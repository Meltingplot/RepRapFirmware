# String heap host test

This test runs RRF's real `src/Platform/Heap.cpp`, `src/Platform/StringHandle.cpp` and `src/Platform/ArrayHandle.cpp`, compiled
unchanged, on a PC. `ExpressionValue` is a stand-in in `shim/ObjectModel/ObjectModel.h`, with the reference counting of the real
one. The test checks what happens when another task copies a string or array handle while the main task deletes it.

That happens with the file name of a machine state:
- The main task deletes the name when a file ends (`GCodeMachineState::CloseFile` runs `fname.Assign(nullptr)`) and when a new
  file starts (`GCodeBuffer::StartNewFile`).
- The SBC task serialises the same handle as `inputs[].currentFile` (`OBJECT_MODEL_FUNC(self->machineState->fname)` in
  `GCodeBuffer.cpp`) in every live object model response. It has a higher priority than the main task.
- Without an SBC the network task makes the same copy, but at the main task's priority with time slicing. The order that this
  test checks does not protect that reader, see `Tools/pointer-check/census.md`.
- `ExpressionValue(AutoStringHandle)` copies the handle without a lock.

The test plays that other task at two moments: when the main task calls `Heap::DeleteSlot` for the handle, and when the call
returns. The hook goes in through the linker option `--wrap`. A handle that still names its slot once the release has started
lets the copy revive a released slot. The slot then has a reference count but no storage, and the next release of it fails the
`RRF_ASSERT` on `storage` or faults reading `storage->length`.

Arrays are copied the same way, by `ExpressionValue(const ExpressionValue&)`. `ArrayHandle::Delete` releases the elements of
the array before the slot itself, so the test also runs the other task at each release of an element. A copy must see the
whole array or none of it.

## Array size

`ArrayHandle::Allocate` computes the size of an array as `sizeof(ArrayStorageSpace) + numElements * sizeof(ExpressionValue)`
(`src/Platform/ArrayHandle.cpp`). On the board, with a 32-bit `size_t` and an 8-byte `ExpressionValue`, `vector(536870912, 0)` makes
that 4. The check of the space then passes, and the loop that constructs the elements writes 2^29 of them beyond a space of 4 bytes
(finding E5 of `Tools/expression-fuzz`; plain G-code is enough, e.g. `echo {vector(536870912, 0)}`). `Allocate` now refuses a count
whose size would wrap around with "Array too large", before it touches the heap.

With the 64-bit `size_t` of the host the same wrap needs 2^64 / `sizeof(ExpressionValue)` elements (2^60 with the 16-byte stand-in), so
the test asks `Allocate` for that count, for 536870912 and for `SIZE_MAX`, and expects "Array too large" for each. Against the source
without the fix, the AddressSanitizer build reports a heap-buffer-overflow at `ArrayHandle.cpp:41` (the construction of the elements),
and the plain build ends with SIGSEGV. The 32-bit wrap itself cannot run on the host (no 32-bit multilib in WSL); that it happens on
the board is from the MB6HC disassembly (`Tools/expression-fuzz/README.md`), not from a board.

The AddressSanitizer build leaves out `-fsanitize=alignment`. The heap rounds every allocation to 4 bytes, which is the
alignment of `ExpressionValue` on the target, but on a 64-bit host the handles in the array elements would need 8.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/heap-test and build/asan-heap-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
Built against handles without the orders of both `StringHandle::Delete` and `ArrayHandle::Delete` (e.g. upstream's), an array
case reads an element string whose slot is already released, and the run ends with SIGSEGV (exit code 139).
