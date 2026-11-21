# Endstop check host test

This test runs RRF's real `Move::OnEndstopOrZProbeStatesChanged` from `src/Movement/Move.cpp` on a PC, with the real `DMState`
(`src/Movement/DriveMovement.h`) and `MovementFlags` (`src/Movement/MoveSegment.h`). The Makefile copies their text out of these
files at build time, as `Tools/kinematics-test` does with `Move::SetKinematics`, because the files as a whole need the hardware
and CAN headers. `endstop_check_test.cpp` gives them a minimal `DriveMovement`, `Move`, `BasePriorityBooster` and
`CanInterface::WakeAsyncSender`, and plays `Move::CheckEndstops`. It is built as for the Duet 3 MB6HC: `SUPPORT_CAN_EXPANSION` and
`SUPPORT_PHASE_STEPPING` are 1.

What the test is about:
- `EndstopsManager::activeEndstops` holds the endstops and Z probes of the current homing or probing move. Only a trigger takes an
  entry out of it (`EndstopsManager::CheckEndstops`). A move that ends without a trigger (G30, G38.x, G1 H1/H3/H4) leaves its
  entries there until the next move that enables endstops rebuilds the list.
- The step ISR checks the list only while a DM whose step is due executes a segment with the `checkEndstops` flag
  (`Move::StepDrivers`), the phase stepping control loop only while a DM of `phaseStepDMs` in a motion state does
  (`Move::PhaseStepControlLoop`).
- `Move::OnEndstopOrZProbeStatesChanged` is called by the CAN receiver task (`TaskPriority::CanReceiverPriority`) after an
  expansion board reports an endstop, Z probe or GpIn change (`src/CAN/CommandProcessor.cpp`), and by the main task after a
  filament monitor's presence or motion changes on a machine with CAN (`src/FilamentMonitors/FilamentMonitor.cpp`). It shuts out
  the step interrupt and calls `Move::CheckEndstops(true)`. If that finds an entry that reads triggered, `stopAll` stops all
  drives and records the hit (`GCodes::MoveStoppedByZProbe` or `GCodes::RecordEndstopTriggered`), whatever move is executing.
  So a stale entry could stop a print move.

The DM lists hold the DMs of drives without local drivers too: `DDA::Prepare` adds segments for them "to track the position", and
while a segment checks endstops `Move::PrepareForNextSteps` computes each of their steps instead of the rare position updates. So
a homing move executed entirely by remote drivers, and a G1 H1 E move of a tool board extruder (the case of upstream 904e14154),
have a DM in `activeDMs` with `checkEndstops` set while their segments execute.

The cases put DMs in `activeDMs` and `phaseStepDMs` as `DriveMovement::NewSegment` leaves them, with an endstop or Z probe in the
active list that reads triggered, and call the function once, as at an input change:
- No move, a print move on local drivers, one with a tool board extruder and one with phase stepping axes: nothing may be stopped
  and no hit recorded.
- A local homing move, a homing move with remote drivers only, a G1 H1 E move of a tool board extruder, a probing move with local
  and remote drivers, a DM that checks endstops behind DMs that do not, a phase stepped homing move, and a DM of `phaseStepDMs`
  that has changed to a cart state: the move must be stopped and the hit recorded once.
- A homing move whose DMs are still `starting` (`NewSegment` has already copied the segment flags): nothing may be stopped yet.
  The step ISR checks the endstops when the segment is due (`StepDrivers` includes the flags of the due `starting` DM).
- An idle DM with flags that check endstops (`DeactivateDM` leaves them): it must not count.

It also checks that `CheckEndstops` runs with the step interrupt shut out and with `executingMove` true, and that the async sender
is woken once after the base priority has been restored when remote drivers were stopped.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/endstop-check-test and build/asan-endstop-check-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
