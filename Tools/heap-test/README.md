# String heap host test

This test runs RRF's real `src/Platform/Heap.cpp` and `src/Platform/StringHandle.cpp`, compiled unchanged, on a PC. It checks
what happens when another task copies a string handle while the main task deletes it.

That happens with the file name of a machine state:
- The main task deletes the name when a file ends (`GCodeMachineState::CloseFile` runs `fname.Assign(nullptr)`) and when a new
  file starts (`GCodeBuffer::StartNewFile`).
- The SBC task, or a network task without an SBC, serialises the same handle as `inputs[].currentFile`
  (`OBJECT_MODEL_FUNC(self->machineState->fname)` in `GCodeBuffer.cpp`) in every live object model response.
- `ExpressionValue(AutoStringHandle)` copies the handle without a lock.

The test plays that other task at two moments: when the main task calls `Heap::DeleteSlot` for the handle, and when the call
returns. The hook goes in through the linker option `--wrap`. A handle that still names its slot once the release has started
lets the copy revive a released slot. The slot then has a reference count but no storage, and the next release of it fails the
`RRF_ASSERT` on `storage` or faults reading `storage->length`.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/heap-test and build/asan-heap-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
