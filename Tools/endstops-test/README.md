# Endstops host test

This test runs RRF's real `EndstopsManager::AddToActive`, `EnableZProbe`, `CheckEndstops`, `SetZProbeDefaults` and `HandleM558`
from `src/Endstops/EndstopsManager.cpp`, and `DeleteObject` from `src/RepRapFirmware.h`, on a PC. The Makefile copies the function
text out of these files at build time, as `Tools/kinematics-test` does with `Move::SetKinematics`, because the files as a whole
need the hardware and CAN headers. `endstops_test.cpp` gives them a minimal `EndstopsManager`, `ZProbe`, `GCodeBuffer`, `GCodes`
and `reprap`. It is built as for the Duet 3 MB6HC: `SUPPORT_CAN_EXPANSION` and `SUPPORT_SCANNING_PROBES` are 1, `MaxZProbes` is 8.

The test checks whether the step ISR or the CAN receiver task can reach a deleted Z probe through `activeEndstops`, the list of
the endstops and Z probes of the current move:
- `EnableZProbe` puts the probe in the list before a probing move (G30, G38.x, ...). Only `CheckEndstops` takes it out again, when
  the probe triggers. A probing move that ends without a trigger leaves it there until the next move that enables endstops
  rebuilds the list.
- The step ISR walks the list while a move that checks endstops executes (`Move::CheckEndstops`, `src/Movement/Move.cpp`). The CAN
  receiver task (`TaskPriority::CanReceiverPriority`) walks it after every endstop, Z probe or GpIn state change that an expansion
  board reports (`src/CAN/CommandProcessor.cpp` -> `Move::OnEndstopOrZProbeStatesChanged`), and the main task does the same after
  a filament monitor change (`src/FilamentMonitors/FilamentMonitor.cpp`), but only while a segment that checks endstops executes
  (upstream 190e3f174). The test plays that reader; `Move::OnEndstopOrZProbeStatesChanged` itself is not compiled in.
  The step ISR and the CAN receiver call `CheckTriggered`, a virtual function, on every entry, and both can run at any instruction
  of the main task.
- M558 with `P` (`HandleM558`) and M502 (`SetZProbeDefaults`) delete Z probes in the main task. M558 with `C` but without `P` is
  rejected before the deletion. Both lock movement and wait for standstill first, so the deleted probe may stay in the list:
  no reader walks it before the next move that checks endstops has rebuilt it.

The test plays these readers while a probe is deleted (in its destructor, before and after its memory is freed) and at the next
remote input change, after a G30 or G38.4 that ended without a trigger and while a G30 probing move of another channel executes.
It never calls a deleted object itself: the reader first compares each list entry with the probes deleted so far, and their
memory is kept until the case ends. It also checks that the next probing move after M558 no longer reaches the old probe, that
M558 without `P` or `C` still changes the probe at once, and that a probing move with the new probe stops when that probe
triggers.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/endstops-test and build/asan-endstops-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
