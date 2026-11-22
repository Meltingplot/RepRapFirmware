# Failed macro host test

This test runs RRF's real handling of a macro that DSF could not run on a PC. DSF answers a macro request with `MacroCompleted` and
an error when it cannot open the file (DSF `Processor.DoMacroFile` sets `MacroError` when `FileFactory.CreateMacro` returns null). When
the G-code buffer waits for the macro, the error ends the wait. When it does not wait, the macro has already been pushed on the stack:
the Daemon channel requests `daemon.g` without waiting (`GCodeBuffer::RequestMacroFile`), and `GCodes::DoFileMacro` pushes a level for
it, so a missing `daemon.g` ends this way. That level must be popped with `CloseFile`, `PopState(false)` and `Init`.

The test checks which task does that:
- The main task (`TaskPriority::SpinPriority`) reads the stack of every G-code buffer without the buffer mutex: M122 walks every
  level (`GCodes::Diagnostics` calls `GCodeBuffer::Diagnostics` for every channel), and the object model reads `inputs[]` through
  `machineState`.
- The SBC task (`TaskPriority::SbcPriority`) has the higher priority, so it can run at any instruction of such a read. If it pops the
  level there, the read goes on with a level that has been deleted. So only the main task may pop it: the SBC task records the failed
  macro on the buffer, under its mutex, and the main task pops it at the start of its next spin (`SbcInterface::ApplyRecordedAborts`),
  as it applies a recorded abort.
- Until then, the SBC task requests packets of the channel again that act on the buffer, and `SbcInterface::FillBuffer` hands no code
  of the channel to the buffer, so that what DSF sends after the error still sees the stack without the macro.

Compiled from the real text, which the Makefile extracts at build time with `extract.awk`, with `#line` directives that point at the
real lines (the build fails if one is missing):
- `src/SBC/SbcInterface.cpp`: the `case SbcRequest::MacroCompleted` of `ExchangeData`, `RecordChannelAbort`, `ApplyRecordedAborts`
  and `FillBuffer`.
- `src/GCodes/GCodeBuffer/GCodeBuffer.cpp`: `PopState`, `CurrentFileMachineState` and `Diagnostics`.
- `src/GCodes/GCodeMachineState.cpp`: `CloseFile` and `Pop`.

`macro_error_test.cpp` gives them minimal `GCodeBuffer`, `GCodeMachineState`, `SbcInterface`, `GCodes`, `reprap`, mutex and string
classes. A level that is deleted keeps its memory until the end and is remembered, so that every later use of it is reported. The SBC
task is played by switching the current task around a call of the real case; in the first case it runs inside the main task's M122
walk, when the walk reads the state of the macro's level. `GCodeBuffer::machineState` is the real `PublishedPtr` of
`src/Platform/PublishedPtr.h` with the main task as its writer (`shim/` stands in for the RTOS interface it uses), so a push or pop by
the SBC task and the deletion of a level that is still on top of the stack fail its `RRF_ASSERT`s, which the test reports.
`Tools/published-ptr-test` tests `PublishedPtr` itself.

Cases:
1. M122 walks the stack while the SBC task processes the error: the walk never reaches a deleted level.
2. The SBC task records the failed macro under the buffer mutex, asks the main task for the apply and changes nothing on the stack.
3. At the start of its next spin the main task takes the record, closes the file, deletes the level once and initialises the buffer;
   a further apply does nothing.
4. A further `MacroCompleted` of the channel is requested again until the pop, and then processed once.
5. A code that the SBC task stores after the error reaches the buffer only after the pop.
6. An abort that DSF sends after the error is applied after the pop, in the same spin.
7. to 10. Unchanged paths: a recorded abort or a busy buffer mutex has the packet requested again, a buffer that waits gets the error as
   the end of the wait, a `MacroCompleted` without an error marks the macro as finished.

What the test does not show: the timing of the real tasks (the preemption is played at one point), the object model readers (M122
stands for them), and the other packets that keep off the buffer while a record is pending (`Lock`, `Unlock`, `EvaluateExpression`,
`SetVariable`, `DeleteLocalVariable`, `SetLastCodeResult`, the reply of a code executed on the SBC, `PrintStopped`), which use the same
check as case 4 but are not compiled here. `GCodes::Spin` is played by `MainTaskSpinStarts`, which calls `ApplyRecordedAborts` when
`ChannelAbortRecorded` has been called.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/macro-error-test and build/asan-macro-error-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason and the events of the case. The exit code is 1 if any case fails.
