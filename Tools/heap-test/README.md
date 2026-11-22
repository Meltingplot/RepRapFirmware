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

The AddressSanitizer build leaves out `-fsanitize=alignment`. The heap rounds every allocation to 4 bytes, which is the
alignment of `ExpressionValue` on the target, but on a 64-bit host the handles in the array elements would need 8.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/heap-test and build/asan-heap-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
