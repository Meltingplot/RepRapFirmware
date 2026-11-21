# Laser task deletion host test

This test runs RRF's real `Move::Exit` and `Move::WakeLaserTask` from `src/Movement/Move.cpp` and `DeleteObject` from
`src/RepRapFirmware.h` on a PC. The Makefile copies their text, and the definition of `Move::laserTask`, out of the sources at
build time, as `Tools/kinematics-test` does with `Move::SetKinematics`, because `Move.cpp` as a whole needs the hardware headers
of CoreN2G. `laser_task_test.cpp` gives them a minimal `Move`, `TaskBase`, `Task`, `StepTimer`, `SmartDrivers` and `DDARing`.

## What is tested and why

`RepRap::EmergencyStop` calls `Move::Exit`, which deletes the laser task (`Move::laserTask`). The `TaskBase` destructor
terminates the task (`TerminateAndUnlink` in `RRFLibraries/src/RTOSIface`: clear `taskId`, unlink it, `vTaskDelete`), then its
memory is freed. `EmergencyStop` runs in:
- the main task (`TaskPriority::SpinPriority`), e.g. M112 in `GCodes::DoEmergencyStop`, an M112 trigger, M999;
- the SBC task (`TaskPriority::SbcPriority`) for `SbcRequest::EmergencyStop` and `SbcRequest::Reset`, and in
  `SbcInterface::EndConnection` when a connection ends;
- the CAN receiver task in expansion mode (`CommandProcessor.cpp`).

`EmergencyStop` has no guard against a second call, so a higher priority task can run `Move::Exit` again while a lower priority
one is inside the deletion, e.g. the SBC task preempting M112 in the main task. Its `Move::Exit` reads `laserTask`. While the
pointer still names the task, that second `Exit` deletes it again: a second free of the same memory, or a destructor on freed
memory.

`WakeLaserTask` only checks the pointer ("must check!"). It gives the task a notification, which in FreeRTOS
(`xTaskGenericNotify` in `tasks.c`) writes into the task control block and puts a task that waits for a notification back on the
ready list, also after `vTaskDelete`. The Move task (`TaskPriority::MovePriority`) calls it from `DDARing::Spin` when it starts a
move. In the code read for this fix the Move task cannot do that during an emergency stop: `EmergencyStop` sets `stopped` before
it calls `Move::Exit`; the Move task waits for work only at the end of its loop in `Move::MoveLoop`, and at the top of the loop it
checks `reprap.IsStopped()` and terminates itself before it fetches or starts a move. The mutexes it takes within the loop
(malloc, the CAN stop list) are held by tasks that inherit its priority and do not wait while they hold them, so a lower priority
emergency stop cannot run in the middle of the loop. The test still plays it, because it reads the same pointer.

The step interrupt runs `ScanningProbeTimerCallback`, which uses `laserTask` without a check. `Move::Exit` disables it first.

The test plays these readers at the three moments of the deletion at which another task can run: after `vTaskDelete`, while
the destructor runs; before the memory is freed; after it has been freed. At each moment `Move::laserTask` must already be null.
It also checks that the step interrupt is disabled before the deletion, that the laser task is freed exactly once, that the Move
task is terminated, and that `Exit` without a laser task and `WakeLaserTask` with a running one work.

The test cannot play a task that runs between the two instructions of `std::swap` in `DeleteObject`, which reads the pointer and
then clears it.

## Shims

- `TaskBase` and `Task`: `taskId`, the destructor that calls `TerminateAndUnlink`, `Give` and `IsRunning`, as in
  `RTOSIface.h`. `Give` and `TerminateAndUnlink` first compare the pointer with the deleted and the freed task, so the test
  reports the use of a deleted task instead of touching it. A class `operator delete` plays the moments before and after the
  free.
- `StepTimer::DisableTimerInterrupt` records that the step interrupt is off; `CancelCallback`, `SmartDrivers::Exit` and
  `DDARing::Exit` do nothing.
- `Move`: only the members that `Exit` and `WakeLaserTask` use, with the stack sizes from `Move.h`.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/laser-task-test and build/asan-laser-task-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
