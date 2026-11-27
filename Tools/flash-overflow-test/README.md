# Flash overflow linker test

This test checks that each RRF linker script stops the link when the firmware image does not fit in the flash.

## Why

A build places the initialised data in `.relocate`, with `.relocate : AT (_etext)` and no memory region for the load address. The
linker therefore checks `.text`, `.ARM.extab` and `.ARM.exidx` against the `rom` region, but not the load image of `.relocate`
behind them. The build appends a CRC32 at `_firmware_crc`, which also lies behind it. If the code fits and the data does not, the
link succeeds and the image runs past the end of the flash.

This happened on 2026-11-27 with the upstream merge of 3.7.1-rc.1. The MB6HC image was 1,048,808 bytes. `.relocate` was loaded
at 0x004ffeb8 to 0x00500090, beyond the end of the 1 MB flash at 0x00500000. The CRC came after that, and the link reported no
error.

The SAM4E script has had a check since e983896d5 (David Crocker, 2022):
`ASSERT(_firmware_crc + 4 <= ORIGIN(rom) + LENGTH(rom), ...)` (`src/Hardware/SAM4E/sam4e8e_flash.ld`). The fix copies it into
every other script that sets `_firmware_crc`. These are the SAM4S, SAME5x and SAME70 scripts, including those of the release
boards:
- `same70q20b_flash.ld` for MB6HC and MB6XD;
- `same54p20a_flash_16k_bootloader.ld` for Mini5plus (`Boards.cmake:40-51`, `Makefiles/*.mk`).

## What the test does

`flash_overflow_test.sh` handles each script on its own. It reads the end of `rom` from the script's MEMORY command, then links an
assembler object with the cross toolchain. The object has a chosen number of code bytes (`.text.fill`) and 256 bytes of
initialised data (`.data.fill`). It links three images:
- **fits:** the CRC ends at most 7 bytes before the end of `rom`. The link must succeed. A probe link with 4 KB of code first finds
  where `_firmware_crc` lands, and the code size is then set from it.
- **crc:** the same image 8 bytes larger, so the CRC ends beyond `rom`. The link must fail with "overflowed flash memory".
- **data:** code that ends 64 bytes before the end of `rom`, followed by 256 bytes of data. The load image of `.relocate` crosses
  the end, as in the MB6HC image. The link must fail with the same message.

By default it checks every script under `src/Hardware` that sets `_firmware_crc`. `SCRIPTS=` selects others, for example the
scripts from before the fix.

## How to run it

The test needs the ARM toolchain, so run it in the firmware build image. From the folder above `RepRapFirmware`:

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" -w /work/RepRapFirmware/Tools/flash-overflow-test dsf-firmware-build make check

With the fix all 36 checks pass, 4 for each of the 9 scripts. With the scripts from before the fix (37d7c3753^), the crc and data
cases fail for the 8 scripts without the check, 16 checks in all. SAM4E passes in both runs.

The firmware builds do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides
it from Eclipse.

## Not covered

- Embedded files: boards built with `RRF_EMBEDDED_FILES` place them at `_firmware_end`. The scripts say that `_firmware_crc` is the
  CRC's offset only when no embedded files are appended. The check therefore covers the image without them, as the SAM4E check
  does.
- RAM: `.bss`, `.ram_nocache` and the stack are checked by the linker through their `> ram` regions.
