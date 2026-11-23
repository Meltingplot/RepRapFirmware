# Driver data host test

This test runs the driver data accessors of `ExpansionManager` from `src/CAN/ExpansionManager.cpp` on a PC:
`StoreDriverDirection`, `StoreDriverMode`, `GetDriverDirection` and `GetDriverMode`. The Makefile copies their text out of
`ExpansionManager.cpp` at build time, as `Tools/kinematics-test` does with `Move::SetKinematics`, because the file as a whole needs
the CAN and hardware headers. `driver_data_test.cpp` gives them a minimal `ExpansionManager`, `ExpansionBoardData`,
`DriverData` and `ReadWriteLock`.

The test checks what the CAN receiver task can do while one of these functions uses a board's `driverData` array:
- `ProcessAnnouncement` runs in the CAN receiver task (`TaskPriority::CanReceiverPriority`). When a board announces itself with a
  different board type or firmware version, e.g. after `M997 B` updated it, it deletes the board's `driverData` array and
  allocates a new one, under a write lock on `boardsLock`.
- `M569` on a remote driver stores the direction (`S`) and mode (`D`) with `StoreDriverDirection` and `StoreDriverMode` in the
  main task (`TaskPriority::SpinPriority`). `Move::GetDirectionValue` reads the direction with `GetDriverDirection`.
  `GetDriverMode` has no caller; it takes the read lock like the others for uniformity.
- The receiver task has the higher priority, so it can run at any instruction of these functions.

The test plays the receiver task at the moment the function uses an element of the array, and fails if the receiver task could
take the write lock then. It also checks that the values arrive, that a board without drivers is left alone, and that the lock is
free again afterwards.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build build/driver-data-test and run it

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
