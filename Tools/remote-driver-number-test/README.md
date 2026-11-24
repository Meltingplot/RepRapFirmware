# Remote driver number host test (expansion mode)

This test checks the driver numbers of two CAN requests that RRF handles when a Duet 3 Mini 5+ or MB6XD runs as an expansion board
(M954): set-driver-states and stop-drivers. Both carry a 16-bit bitmap of drivers.

## Why

`Move::EutHandleSetDriverStates` (`src/Movement/Move2.cpp`) used every bit of `driversToUpdate` as a driver number without a check:
- it read `motorCurrents[driver]` and wrote `driverState[driver]`, which have `MaxAxesPlusExtruders` entries (`src/Movement/Move.h`);
- on the Mini 5+, `MaxAxesPlusExtruders` is 12 and `NumDirectDrivers` is 7 (`src/Config/Pins_Duet3Mini.h`), so drivers 12 to 15 read
  and write beyond the two arrays, and drivers 7 to 11 change the state of drivers that the board does not have;
- the values of drivers 8 to 15 are read beyond the declared `values[8]` of the message, but within its 64 bytes.

`Move::StopDriversFromRemote` passed every bit to `StopDriveFromRemote`, which indexes `dms[]`. That stays within `dms[]`, but bits 7
to 15 stop drive movements that are not local drivers.

The other multiple-drives handlers of the same file (steps per mm and microstepping, motor currents, standstill currents) already
refuse a driver number of `NumDirectDrivers` or more with "No such driver". Duet3Expansion got the same check for its copies of both
handlers in 162803de (C. Hammacher, 2026-11-13, "Driver numbers in set-driver-states and stop-drivers requests are checked against the
number of drivers"). The fix does the same in RRF: set-driver-states reports "No such driver" and an error for each such driver and
handles the others, and stop-drivers leaves it out.

Upstream code. A main board sends these requests for the drivers that the configuration names, so a wrong driver number in the
configuration of the main board is enough; whether the main board checks remote driver numbers before it sends them was not traced.
Found by the census of CAN receive (error class 3, 2026-11-24). Not seen on a board.

## What the test compiles

The real text of `Move::EutHandleSetDriverStates` and `Move::StopDriversFromRemote`, which the Makefile extracts with
`../macro-error-test/extract.awk` (with `#line` directives that point at the real lines), and the real `NumDirectDrivers` and
`MaxAxesPlusExtruders` of the Mini 5+. `remote_driver_number_test.cpp` gives them a `Move` stand-in whose `driverState` and
`motorCurrents` are heap arrays of exactly `MaxAxesPlusExtruders` entries, so that AddressSanitizer sees an access beyond them, and
whose driver functions record their calls. `DriverStateControl` and `CanMessageMultipleDrivesRequest` are copies of those in CANlib
`src/CanMessageFormats.h`; RRFLibraries `General/Bitmap.h` is the real one.

Six cases: every local driver in each of the three modes, all 16 drivers in each mode, driver 12 alone, and stop-drivers for all 16.
Without the fix 5 fail, and the AddressSanitizer build stops with a heap-buffer-overflow at `Move2.cpp:1736` (`motorCurrents[driver]`
for driver 12). With the fix all 6 pass.

Not covered: the main board side, the MB6XD sizes, and what the drivers themselves do.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

The Makefile reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The firmware builds
do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
