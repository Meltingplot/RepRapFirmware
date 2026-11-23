# Z probe timer host test

This test runs RRF's real Z probe programming code and step timer code on a PC: the class declaration of `LocalZProbe`
(`src/Endstops/LocalZProbe.h`), its destructor, `SendProgram`, `TimerInterrupt` and `Interrupt` (`src/Endstops/LocalZProbe.cpp`),
the constructor, `SetCallback`, `ScheduleCallback`, `ScheduleCallbackFromIsr`, `CancelCallback`, `CancelCallbackFromIsr` and
`Interrupt` of `StepTimer` (`src/Movement/StepTimer.cpp`, with the real `src/Movement/StepTimer.h`), `EndstopsManager::ProgramZProbe`
(M672) and `SetZProbeDefaults` (M502) (`src/Endstops/EndstopsManager.cpp`), and `DeleteObject` (`src/RepRapFirmware.h`). The
Makefile copies the text out of these files at build time, as `Tools/kinematics-test` does with `Move::SetKinematics`, because the
files as a whole need the hardware headers. `zprobe_timer_test.cpp` gives them a minimal `ZProbe`, `IoPort`, `GCodeBuffer` and
`EndstopsManager` and the free list of RRFLibraries (`General/FreelistManager.h`); `shim/RepRapFirmware.h` gives `StepTimer.h` what
it needs. It is built as for the Duet 3 MB6HC: `SUPPORT_CAN_EXPANSION` and `SUPPORT_SCANNING_PROBES` are 1, the step clock runs at
750kHz.

The test checks whether the step ISR can call back a Z probe that the main task has deleted:
- M672 (`ProgramZProbe` -> `SendProgram`) writes the first bit, schedules the probe's `StepTimer` and returns at once. The step
  ISR then calls `LocalZProbe::Interrupt` once per bit at 1000 bits per second (`bitsPerSecond`, `LocalZProbe.h:58`), 14 bit
  times per byte, so up to 112ms for the 8 bytes that M672 accepts. The timer stays in `StepTimer::pendingList` all that time.
- M558 with `P` deletes the probe (`HandleM558`) and M502 deletes all probes (`SetZProbeDefaults`), in the main task. The wait
  for standstill that both do first (`HandleM558`, and M502 in `GCodes2.cpp`) does not wait for the program: sending it is not a
  move.
- `StepTimer` has no destructor. If `~LocalZProbe` leaves the timer in the pending list, the step ISR calls `TimerInterrupt` with
  the deleted probe. If M558 creates a new `LocalZProbe`, the free list hands it the memory of the old one (last in, first out per
  object size), and the `StepTimer` constructor sets `next` and `callback` to null while the timer is still in the pending list:
  the step ISR would call a null pointer, and the timers behind it, e.g. `Move::stepsTimer`, drop out of the list.

The test plays the step ISR between the statements of the main task, at every compare match that the step timer code sets up
(`ScheduleTimerInterrupt` as in `StepTimer.cpp:227-250`, with the step clock of the test). Before every interrupt it walks the
pending list. If a timer in it lies in memory on a free list or has no callback, the case fails and the ISR stops for the rest of
the case, so the test never calls a deleted probe or a null callback; the free list keeps the memory of the deleted probes, as the
firmware's does. Another client of the step timer stands for `Move::stepsTimer` and must be called back once. The cases:
- M672 returns before the program is sent, and the step ISR sends the documented bit pattern in 14 bit times per byte.
- M672 while a program is being sent starts the new one (`SendProgram` cancels the timer first, `LocalZProbe.cpp:155`).
- M558 P5 C replaces the probe while M672 is sending (the new probe gets the same memory).
- M558 P deletes the probe while M672 is sending (its memory stays on the free list).
- M502 deletes two probes while M672 is sending to both.
- M558 P5 C replaces the probe after the program has been sent.

No two timers are due at the same tick in these cases: the M502 case starts the second M672 100 ticks after the first, and the
other client is due between the bit times of the probes. Two timers that both use the due check of `StepTimer.h:16-19` and are
due at the same tick make `StepTimer::Interrupt` run them in turn forever, because `ScheduleCallbackFromIsr()` puts a timer behind
a pending one that is due at the same tick and returns false (`StepTimer.cpp:508-535`). Only `LocalZProbe` uses that check, so
this needs two local probes whose programs are sent with coinciding bit times. It is not part of this test; the test fails with
"the step ISR ... does not end" if one interrupt runs more than 1000 timers.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/zprobe-timer-test and build/asan-zprobe-timer-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
