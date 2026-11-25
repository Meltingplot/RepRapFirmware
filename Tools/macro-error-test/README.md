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
  level there, the read goes on with a level that has been deleted. So only the main task may pop it: the SBC task requests the pop on
  the buffer (`SetFailedMacroPending`), under its mutex, and the main task pops it at the start of its next `GCodes::SpinGCodeBuffer`
  of that buffer, as upstream does.
- Until then, the SBC task requests packets of the channel again that act on the buffer (`IsRecordPending`, as for a recorded abort),
  and `SbcInterface::FillBuffer` hands no code of the channel to the buffer, so that what DSF sends after the error still sees the
  stack without the macro.
- An abort of the channel that comes in before the pop pops the macro with the other files (`AbortFile` in
  `SbcInterface::InvalidateChannel`) and clears the pending pop, so that the main task does not pop a further level.

Compiled from the real text, which the Makefile extracts at build time with `extract.awk`, with `#line` directives that point at the
real lines (the build fails if one is missing):
- `src/SBC/SbcInterface.cpp`: the `case SbcRequest::MacroCompleted` of `ExchangeData`, `RecordChannelAbort`, `ApplyRecordedAborts`,
  `InvalidateChannel` and `FillBuffer`.
- `src/GCodes/GCodes.cpp`: the pop of a failed macro at the start of `SpinGCodeBuffer` (the block `if (gb.IsFailedMacroPending())`).
- `src/GCodes/GCodeBuffer/GCodeBuffer.cpp`: `PopState`, `CurrentFileMachineState`, `AbortFile` and `Diagnostics`.
- `src/GCodes/GCodeMachineState.cpp`: `DoingFile`, `CloseFile` and `Pop`.

`macro_error_test.cpp` gives them minimal `GCodeBuffer`, `GCodeMachineState`, `SbcInterface`, `GCodes`, `reprap`, mutex and string
classes. A level that is deleted keeps its memory until the end and is remembered, so that every later use of it is reported. The SBC
task is played by switching the current task around a call of the real case; in the first case it runs inside the main task's M122
walk, when the walk reads the state of the macro's level.

Cases:
1. M122 walks the stack while the SBC task processes the error: the walk never reaches a deleted level.
2. The SBC task requests the pop of the failed macro under the buffer mutex and changes nothing on the stack.
3. In its next spin of the buffer the main task closes the file, deletes the level once, initialises the buffer and clears the pop;
   a further spin does nothing.
4. A further `MacroCompleted` of the channel is requested again until the pop, and then processed once.
5. A code that the SBC task stores after the error reaches the buffer only after the pop.
6. An abort that DSF sends after the error pops the macro with the other files and clears the pending pop, in the same spin; the
   base level stays and the pop does not run again.
7. to 10. Unchanged paths: a recorded abort or a busy buffer mutex has the packet requested again, a buffer that waits gets the error as
   the end of the wait, a `MacroCompleted` without an error marks the macro as finished.

What the test does not show: the timing of the real tasks (the preemption is played at one point), the object model readers (M122
stands for them), and the other packets that keep off the buffer while a record is pending (`Lock`, `Unlock`, `EvaluateExpression`,
`SetVariable`, `DeleteLocalVariable`, `SetLastCodeResult`, the reply of a code executed on the SBC, `PrintStopped`), which use the same
check as case 4 but are not compiled here. `GCodes::Spin` is played by `MainTaskSpinStarts`, which calls `ApplyRecordedAborts` when
`ChannelAbortRecorded` has been called and then runs the start of `SpinGCodeBuffer` for every buffer. `GCodes::AbortStateMachine` and
the other calls of `InvalidateChannel` that do not touch the stack are stand-ins.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/macro-error-test and build/asan-macro-error-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason and the events of the case. The exit code is 1 if any case fails.
