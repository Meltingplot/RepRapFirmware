# Memory barrier before XDMAC starts check

This check fails when the SAME70 firmware (`Duet3_MB6HC`, `Duet3_MB6XD`) can start an XDMAC channel without a memory barrier on
the way there, unless `allowlist.txt` names the place.

## Why

On the SAME70 the buffers that a DMA reads are in the non-cached RAM area (CoreN2G `src/Cache.cpp`, MPU table in `Cache::Init`;
`Cache::Flush` asserts it). That area is Normal memory and the peripherals are Device memory. ARMv7-M does not keep a write to
Normal memory ordered before a later write to Device memory, so the write that starts the DMA can take effect before the last
writes to the buffer have reached RAM. The DMA then sends old bytes. A `dsb` (or `dmb`) between the two orders them.

Upstream needed this on the SAME70 for the CAN controller: CoreN2G 9e6afc3 (D. Crocker) added the `__DSB()` before the write to
`TXBAR` (`src/CanDevice.cpp:779`). On the processors without a data cache, `Cache::Flush` is exactly this barrier
(`src/Cache.h:41`, `:62`). Before Meltingplot/CoreN2G#5, the SAME70 version of `Cache::Flush` (`src/Cache.cpp:384-396`) only
checked the buffer address. A DMA start that ran
`Cache::FlushBeforeDMASend` on its buffer therefore had no barrier:
- the SBC SPI transfers (`src/SBC/DataTransfer.cpp`, `ExchangeHeader`, `ExchangeResponse`, `ExchangeData` and the guarded
  transfers);
- the WiFi SPI transfers (`src/Networking/ESP8266WiFi/WiFiInterface.cpp`, `SendCommand`);
- the SD card block transfers (CoreN2G `src/SAM4S_4E_E70/asf/sam/drivers/hsmci/hsmci.c:709`, `:812` and the others).

The LED strips (`src/LedStrips/LocalLedStrip.cpp`, `DmaSendChunkBuffer`) did not run it at all.

Found by the barrier census of 2026-11-23 (F1 for SBC and WiFi, F2 for the LED strips), recorded in DuetSoftwareFramework
`design/open-findings.md`. The failure has not been seen on a board.

## What counts

**A start** is a store to `XDMAC_GE` (0x4007801C), the global channel enable register, which `xdmac_channel_enable` writes
(CoreN2G `src/SAM4S_4E_E70/asf/sam/drivers/xdmac/xdmac.h:362`). The check finds the address by following the constants in the registers in
address order: literal loads, `mov`, `movw`/`movt` and `add`/`sub` of an immediate. A call forgets the registers that it may
change.

**A barrier** is a `dsb` or `dmb`, or a call to a function that runs one on every path through it that returns. A function that
does not return (`vAssertCalled`) ends the path. `Cache::Flush` counts after the fix because its only return runs the `dsb`.

**Ordered** means that every path from the entry of the function to the start runs a barrier. If a path does not, every direct
call of the function (`bl`, or a tail call with `b`) must be ordered in its caller in the same way, and so on up the callers, at
most 8 levels. The control flow (branches, IT blocks, `tbb`/`tbh` jump tables, tail calls, veneers, functions that do not
return) comes from the program model of `Tools/alloc-check` (`alloc-check.py`, class `Program`), which this check imports.

## Key format

    Start <- Caller <- ...

The functions from the one with the store up to the first one without a direct caller or the depth limit, without their
parameter lists, as `Tools/alloc-check` writes them. Each chain without a barrier is one finding. Each new finding gives one line
in the format that compilers use, with the outermost source line of the store:

    src/LedStrips/LocalLedStrip.cpp:367: error: XDMAC channel started without a memory barrier before it: LocalLedStrip::DmaSendChunkBuffer <- ...

The exit code is 1 if there is a new finding. `--verbose` also prints every start that the check found and the allowed findings.

## What the check does not see

- **Where the barrier is.** A barrier anywhere before the start counts, also one before the buffer writes. On the ELF before the
  fix, the SBC transfers that `SbcInterface::TaskLoop` starts through `DataTransfer::DoTransfer` count as ordered, because
  `TaskLoop` called `delay` (whose task switch runs a `dsb`) before it. Only the chains through `StartNextTransfer` were found
  there. The machine code does not tell a write to a DMA buffer from any other write (after `Cache::FlushBeforeDMASend`,
  `ExchangeResponse` still writes the transfer state), so the check cannot require the barrier after the last buffer write.
  The convention that it relies on is the one the firmware follows: `Cache::FlushBeforeDMASend` on the buffer, then the start.
- **Calls through a register.** A barrier in a function called through a pointer does not count, and a start in such a function
  is checked up to the function only (key without callers).
- **Other DMA controllers.** The GMAC, the USB device DMA and the MCAN message RAM have their own start registers. The census
  found barriers there (`gmac.h`, `dcd_samx7x.c`, `CanDevice.cpp:779`).
- **The SAME5x and SAM4E builds.** Their `Cache::Flush` is a `__DSB()` (`src/Cache.h:62`). The census took their Cortex-M4 as
  keeping these writes in order (ARM AN321, section 3.5); this check does not look at them.

## Build and run

In the firmware build container of DuetSoftwareFramework (Arm GNU toolchain and python3):

    make check                      # build Duet3_MB6HC and Duet3_MB6XD with -g, then check them
    make scan                       # check the ELF files that are there
    make selftest                   # check dma-barrier-check.py against selftest/src/sample.cpp

`selftest/src/sample.cpp` has starts with a barrier in the function, in a function it calls, on both branches and through a tail
call, and starts without one: through a function without a barrier, through one with a barrier on one path only, with the
barrier on one branch only, inline, and in a function reached through a pointer only. `selftest/expected.txt` lists the keys
that the check must report.

Against the ELF of `Duet3_MB6HC` built with CoreN2G before the fix (and without the LED strip fix), the check reports 28
chains: the 2 of the SBC transfers through `StartNextTransfer`, 22 of the WiFi transfers through `SendCommand`, the 2 SD card
transfers and the 2 LED strip types. With Meltingplot/CoreN2G#5 and the LED strip fix it reports nothing for `Duet3_MB6HC` and
`Duet3_MB6XD`, so `allowlist.txt` is empty.

The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from
Eclipse.
