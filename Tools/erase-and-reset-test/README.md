# EraseAndReset host test

This test checks that `EraseAndReset` on the SAME70 unlocks every lock region of the flash with a bounded number of flash commands
between two watchdog kicks, before it tells the chip to boot from ROM (SAM-BA) and resets.

M999 P"ERASE" resets with `SoftwareResetReason::erase` (`src/GCodes/GCodes2.cpp:4892-4894`), and `SoftwareReset` calls
`EraseAndReset` for it on the SAME70 (`src/Hardware/ExceptionHandlers.cpp:41-48`). `EraseAndReset` (CoreN2G `src/SAM4S_4E_E70/Reset.cpp`) runs with interrupts off
and kicks the watchdog once per flash page.
- It came from CoreNG (`cores/arduino/Reset.cpp`), where it called the ASF `flash_unlock(start, end, ...)` with
  `pageStartAddr + IFLASH_PAGE_SIZE - 1` as the end of the page.
- CoreN2G acc43dd (2021-01-15) ported it to `Flash::Unlock`, which takes a start and a length (`src/Flash.h`,
  `src/SAM4S_4E_E70/Flash.cpp`), but kept the end address as the second argument.
- So each call asks to unlock from the page to about twice its address. `Flash::Unlock` issues a Clear Lock Bit command for each
  lock region from the page's region up to that end, or up to the first command that fails. On the SAME70Q20B that is up to
  129 commands between two watchdog kicks and about 134000 in all (if a page past the end gives an error), or up to 641 and
  about 1.2 million (if it does not).
- The fix passes `IFLASH_PAGE_SIZE` as the length.

## What the test compiles

The Makefile copies the real text out of CoreN2G at build time (`extract.awk`, with `#line` directives so that messages point at the
real lines; this copy also finds a function whose head starts with `[[noreturn]]`):
- `EraseAndReset` from `src/SAM4S_4E_E70/Reset.cpp`;
- `translate_address`, `compute_lock_range` and `Flash::Unlock` from `src/SAM4S_4E_E70/Flash.cpp`.

`erase_and_reset_test.cpp` gives them stand-ins:
- the flash of the SAME70Q20B of Duet3_MB6HC and Duet3_MB6XD: 1 MiB, 512-byte pages, 8 KiB lock regions (CoreN2G
  `src/SAM4S_4E_E70/asf/sam/utils/cmsis/same70/include/same70q20b.h:675-685`);
- `efc_perform_command`: counts the commands and the lock regions that a Clear Lock Bit command unlocks;
- `wdt_restart`: counts the kicks;
- `Flash::ClearGpNvm` and `rstc_start_software_reset`, which ends the run with `longjmp`;
- `Assert` is empty, as in the firmware builds (ASF `compiler.h:204`).

What the EEFC answers for a page number past the end of the flash is not in what the test rests on, so each run is done twice:
once with an error for such a page, once with success.

Each run checks that all 128 lock regions are unlocked, that GPNVM bit 1 is cleared before the reset, that no flash command runs with
interrupts on, that there are at most 2 flash commands between two watchdog kicks (one for the page's region, one more for the
last page of a region, because `Flash::Unlock` rounds the end up to the next region), and at most 2177 in all.

Without the fix both runs fail: 134145 commands with at most 129 between two kicks, and 1182337 with at most 641.

Not covered:
- how long a Clear Lock Bit command takes, so whether the old code really ran into the watchdog or only took long;
- what SAM-BA does after the reset.

The Makefile reads CoreN2G from next to this repository, `../../../CoreN2G` (override with `COREN2G=`). The firmware builds do not use
this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/erase-and-reset-test and build/asan-erase-and-reset-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against CoreN2G without the fix: `make clean check COREN2G=<checkout of the previous commit>`. The generated
files depend on the source files, not on `COREN2G=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
