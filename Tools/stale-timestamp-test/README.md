# Stale time stamp host test

This test checks the timeout checks that compare a clock reading with a time stamp that a higher-priority task or an ISR writes. They
must still see a fresh stamp as fresh when the writer stores a newer one right after the clock was read.

The checks compute `now - stamp` and compare it with a timeout. If they read the clock first and the stamp afterwards, a writer that
preempts them in between can store a stamp that is newer than `now`. `now - stamp` then wraps to about 2^32, and the check takes the
newest data as timed out. Each load and store is atomic; the order is the error. Upstream fixed this case in
`ExpansionManager::Spin` in 3e89b43bd (D. Crocker, 2023-05-23, "Fixed race condition when checking for expansion board status
timeout"): it reads the stamp before it calls `millis()` (`src/CAN/ExpansionManager.cpp:606-610`). `TemperatureSensor::CheckTimeout`
does the same (`src/Heating/Sensors/TemperatureSensor.cpp:52-56`). The fixed functions copy that order:

| Function | Writer | What the wrong timeout did |
|---|---|---|
| `RemoteHeater::GetMode`, `GetTemperature`, `GetAveragePWM` (`src/Heating/RemoteHeater.cpp`) | `UpdateRemoteStatus`, CanReceiver task | The heater read as offline, so `Heat::HeaterAtSetTemperature` returned true (`src/Heating/Heat.cpp`, offline is neither active nor standby) and M116, M109 or a tool change stopped waiting. The temperature read as the local sensor reading and the PWM as 0 |
| `RemoteFan::Check` (`src/Fans/RemoteFan.cpp`) | `UpdateFromRemote`, CanReceiver task | RPM and PWM set to -1 until the next fan report, and the fan did not count as a running thermostatic fan |
| `LocalFan::GetRPM` (`src/Fans/LocalFan.cpp`) | `Interrupt`, the tacho ISR | RPM 0 for one reading |
| `FilamentMonitor::FilamentPresenceKnown`, `IsFilamentPresent`, `IsMotionDetected` (`src/FilamentMonitors/FilamentMonitor.cpp`) | `UpdateRemoteFilamentStatus`, CanReceiver task | Presence unknown, no filament and no motion for that check |

Found by the shared-data census of 2026-11-23 (F1, F4, F5, F6), which also read the order of the clock call and the load in the
`Duet3_MB6HC` ELF. How often the window is hit on a board was not measured.

## What the test compiles

The Makefile copies the real functions out of `src/` at build time (`extract.awk`, with `#line` directives so that messages point at
the real lines). `stale_timestamp_test.cpp` gives them stand-ins for their classes with the members that they use, and the constants
of the headers named there.

`millis()` and `StepTimer::GetTimerTicks()` return the time that they read, then run the writer that the case has set up, one tick
later. That is the writer preempting the caller right after the clock read.

Each function runs three cases: a report some time ago (fresh), one older than the timeout, and a fresh one plus a newer one that the
writer stores during the check. The first and the last must give the reported values, the second the timed-out ones. That is 24
runs.

Without the fix the 8 runs with a newer report during the check fail; the others pass.

Not covered:
- the other places of the census that were found to be ordered right, or where the writer cannot preempt the reader;
- `SbcInterface` (fork code, census F7) and Duet3Expansion `CanInterface::UpdateSyncLockState` (census F2), which are fixed
  separately.

The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/stale-timestamp-test and build/asan-stale-timestamp-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against the source without the fix: `make clean check RRF=<checkout of the previous commit>`. The
generated files depend on the source files, not on `RRF=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
