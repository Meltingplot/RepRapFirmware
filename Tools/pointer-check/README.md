# Raw pointer check

This check fails when RepRapFirmware declares a new raw pointer that can be reseated, unless `allowlist.txt` names it.

## Why

Other tasks read many pointers without a lock, for example the object model when the SBC task or a network task serialises
it. When the main task deletes an object while such a pointer still names it, the reader follows a pointer to freed memory.
Two cases were fixed in this fork:
- `StringHandle::Delete` released the slot before it cleared the handle (#50).
- `Move::SetKinematics` deleted the old kinematics before it pointed `kinematics` at the new one (#51).

Neither order is visible in the declaration `T *p`. The aim is that every pointer that another task can read states how it is
shared, through a pointer class instead of a raw pointer. This check finds the raw pointers. The pointer classes do not exist
yet, so for now every existing pointer is in `allowlist.txt`.

`census.md` has the census of all entries: who reads and writes each pointer, at which task priority, the verdict that its
comment in `allowlist.txt` gives, and the findings with their fixes.

## What counts

The check counts every class member, static member and variable outside a function whose type is a pointer or an array of
pointers, and whose pointer is not const:
- `T *p`, `const T *p`, `T *volatile p`, function pointers and typedefs of pointers (`TaskHandle`) count.
- `T *const p` and references do not count.
- Declarations inside functions do not count: locals, static locals, local classes and the members of lambda closures.
- Code under `src/` that comes from elsewhere does not count: lwIP, the GMAC driver, MQTT-C, the Wiznet driver,
  `src/Libraries`, BOSSA, `src/libc` and `src/libcpp`. `VENDORED` in `pointer-check.py` lists these folders.

`selftest/src/sample.cpp` has one example of each case. `selftest/src/no-pointers.cpp` has none: `make selftest` runs
`--update` on it, which must write an allowlist with the header and no entries.

## How it works

The check reads the declarations from the DWARF debug information that the compiler writes with `-g`. It uses
`arm-none-eabi-readelf` of the Arm GNU toolchain and python3 and needs no other compiler. It therefore sees what the firmware
build compiled for each configuration, including the files that only the firmware compiler accepts.

`make check` first rebuilds each configuration with `DEBUG_FLAGS=-g`. `-B` rebuilds every object, so no object is left over
from a build without `-g`. `DEBUG=1` would not do, because it also sets `-Og -DDEBUG`. `-g` adds the debug sections and leaves the code
alone. For `Duet3_MB6HC` at 06707c314, the `.bin` with and without `-g` differed only in three places:
- the build time (`IsoDateTime`);
- the CRC at the end;
- the order of the three `__efc_*_veneer` stubs, which jump to the same RAM functions.

What the check does not see:
- A class template that is never instantiated, and a type that no compiled code uses: the compiler writes no debug information
  for them. They are not in the firmware either. The check sees them as soon as code uses them.
- A source file of which the linker keeps no section, e.g. a file that the configuration leaves empty with `#if`: the linker
  (`--gc-sections`) drops its debug information with its code. At 85a1fe59c this applied to 4 of the 176 objects outside the
  `VENDORED` folders in `Duet3_MB6HC`, 13 of 176 in `Duet3_MB6XD` and 18 of 198 in `Duet3Mini5plus`. None of them keeps a
  non-empty section in the linker map, so their code is not in the firmware either.
- Configurations that it does not build. The default is those of `make all` (`Duet3_MB6HC`, `Duet3_MB6XD`, `Duet3Mini5plus`);
  `Duet3_CAN0` and `Duet3_MB6HC_no_SD` are not included.

## Use

The Makefile needs the Arm GNU toolchain (`CROSS_COMPILE`, default `arm-none-eabi-`) and python3. The firmware build
container of DuetSoftwareFramework has both: `firmware/build.sh shell`, then `make -C RepRapFirmware/Tools/pointer-check check`.

    make check                      # build the configurations with -g, then check them
    make check CONFIGS=Duet3_MB6HC  # only this configuration; entries of the others show up as notes
    make scan                       # check the ELF files that are already there, without building
    make update                     # build, then rewrite allowlist.txt from the firmware
    make selftest                   # check pointer-check.py against selftest/src/sample.cpp and no-pointers.cpp

Each new pointer gives one line, in the format that compilers use:

    src/Movement/Move.h:715: error: new raw pointer that can be reseated: Move::pointerCheckProbe (int *). ...

The exit code is 1 if there is a new pointer.

## A new pointer

Use a pointer class that states how the pointer is shared. Until RepRapFirmware has such classes, add the key from the error
message to `allowlist.txt` with the reason why the pointer is safe, e.g. that only one task reads and writes it. A static member
is reported at its definition in the `.cpp` file, not at the declaration in the class. When a pointer goes away, the check
prints a note, and `make update` removes it from the list.

The Meltingplot release workflow (`.github/workflows/mp-release.yml`) runs `make check` before it uploads the binaries. A new
pointer stops the release.

The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from
Eclipse.
