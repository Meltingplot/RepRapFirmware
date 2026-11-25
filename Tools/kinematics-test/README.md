# Kinematics change host test

This test runs RRF's real `Move::SetKinematics` from `src/Movement/Move.cpp`, with `ReplaceObject` from `src/RepRapFirmware.h`, on
a PC. The Makefile copies the function text out of these files at build time, as `Tools/sbc-harness` does with
`SbcInterface::TaskLoop`, because `Move.cpp` as a whole needs the hardware headers of CoreN2G. `kinematics_test.cpp` gives it a minimal `Move`, `Kinematics`, `reprap` and a compile-only
`ObjectModel::lifetimeLock`.

The test checks what another task sees while the kinematics changes:
- M665 and M669 call `SetKinematics` in the main task when the kinematics type changes, e.g. `M669 K5` in `config.g` after the
  default Cartesian kinematics.
- The SBC task serialises `move.kinematics` (`OBJECT_MODEL_FUNC(self->kinematics)` in `Move.cpp`) when it is asked for the
  move key or the whole model. It has a higher priority than the main task (`SbcPriority` and `SpinPriority` in
  `src/Platform/TaskPriorities.h`), so it can run at any instruction of the change.
- Without an SBC the network task serialises it too, but at the main task's priority with time slicing, so the main task can
  also run while that task holds the pointer. The order that this test checks does not protect that reader, see
  `Tools/pointer-check/census.md`.

The test plays that task while the old kinematics is destroyed, just before its memory is freed and just after. At each of these
moments `move.kinematics` must name the new kinematics; the old one is no longer intact. It also checks that `MoveUpdated` runs
after the change, and that an unknown type leaves the kinematics alone.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/kinematics-test and build/asan-kinematics-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
