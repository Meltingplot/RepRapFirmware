# Expansion driver data host test

This test runs the four driver data accessors of `ExpansionManager` (`src/CAN/ExpansionManager.cpp`) and the `HasDriver(s)` members of
`ExpansionBoardData` (`src/CAN/ExpansionManager.h`) on a PC. The Makefile extracts their real text, so the test follows the source.

## Why

When an expansion board announces itself, `ProcessAnnouncement` allocates `driverData` with the number of drivers that the board
reports (`ExpansionManager.cpp:226-238`). `StoreDriverDirection`, `StoreDriverMode`, `GetDriverDirection` and `GetDriverMode` then
index that array with the driver number they are given. They only checked that the board has driver data at all (`HasDrivers()`), not
that the number is below the count. A larger number read or wrote beyond the heap array.

The driver numbers come from G-code:
- `M569 P<board>.<driver> S` and `D` store the direction and mode after the board accepted the command (`GCodes3.cpp:1083-1097`).
  Duet3Expansion refuses a driver that it does not have, so the write needs a board whose reply and announcement disagree.
- `Move::GetDirectionValue` reads the direction of a remote driver (`Move2.cpp:966-973`). The Hangprinter calls it for its configured
  drivers (`HangprinterKinematics.cpp:1133-1142`). It checks the number of a local driver there, not of a remote one. That a
  configuration mistake reaches it is from the census of CAN receive (finding CAN-R2, error class 3, 2026-11-24), not traced again.
- `GetDriverMode` has no caller outside `ExpansionManager` in 3.7-dev.

The local drivers already do this: `SmartDrivers::GetDriverMode` returns `DriverMode::unknown` for a driver at or beyond the count
(`TMC51xx.cpp:2327`, `TMC22xx.cpp:2456`, `TMC2660.cpp:1039`). The fix adds `ExpansionBoardData::HasDriver(driver)`, which is true only
for a driver below `numDrivers`. The four accessors use it in place of `HasDrivers()`, so a driver number that the board did not
announce is treated like a board without driver data. The store does nothing, the direction is forwards, the mode is unknown.

Upstream code, the same in 3.7-dev and 3.7-ch. The read lock in the accessors is fork code (24f46bad7) and stays as it is.

## What the test does

Board 1 announces 3 drivers. The test checks that:
- the direction and mode of drivers 1.0 to 1.2 are stored and read back, and keep their defaults otherwise;
- a board without driver data gives forwards and unknown, as before;
- drivers 1.3 and 1.5 give forwards and unknown;
- storing to drivers 1.4 and 1.7 changes nothing, neither behind the 3 drivers nor in them.

The AddressSanitizer build allocates exactly 3 drivers. The plain build allocates 8 more behind them, set to other values, so that an
access beyond the 3 shows in the results.

Without the fix, the plain build fails 3 of the 10 cases, and the AddressSanitizer build stops with a heap-buffer-overflow in
`ExpansionManager::GetDriverDirection`. With the fix, all 10 pass in both builds.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

To see the failure, run it against the source without the fix: `make clean check RRF=<checkout of the previous commit>`. The firmware
builds do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
