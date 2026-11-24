# USB direct read cache host test

This test checks that a direct read of `SerialCDC` on the SAME70 gives the reader the bytes that the USB DMA stored, also when the
processor filled data cache lines of the buffer while the DMA was writing it.

M559 and M560 uploads over USB receive the file data straight into two 4096-byte buffers (`BufferedStreamGCodeInput`,
`src/GCodes/GCodeInput.cpp`). They are allocated with `Tasks::AllocPermanent`, 32-byte aligned, so they are in cacheable RAM and not in
the non-cached area that the other DMA buffers of the SAME70 use (CoreN2G `src/Cache.cpp`, MPU table in `Cache::Init`).
- `SerialCDC::StartDirectRead` (CoreN2G `src/SerialCDC_tusb.cpp`) hands a buffer to TinyUSB. The SAMx7x device driver cleans and
  invalidates the buffer's cache lines before it starts the DMA (`CleanInValidateCache` in `dcd_edpt_xfer`, LibTinyusb
  `src/tinyusb/src/portable/microchip/samx7x/dcd_samx7x.c`). Nothing invalidates them after the DMA.
- The processor may read Normal memory speculatively, so a line of the buffer can be filled from RAM while the DMA is still writing
  it. The reader then gets the old content of that line. The reader works on one buffer while the DMA fills the other, and the two
  buffers are next to each other.

The fix invalidates the buffer's lines in `SerialCDC::DirectReadXferComplete`, before it tells the reader that the read is complete.
Because that throws away whatever else is in those lines, `StartDirectRead` asserts on the SAME70 that the buffer and the length are
whole cache lines.

## What the test compiles

The Makefile copies the real text out of CoreN2G at build time (`extract.awk`, with `#line` directives so that messages point at the
real lines):
- the class `SerialCDC` from `src/SerialCDC_tusb.h`;
- from `src/SerialCDC_tusb.cpp`: the constructor, `Start`, `GetMaxPacketSize`, `TakeReceiveEndpoint`, `StartDirectRead`,
  `IsDirectReadComplete`, `ReturnReceiveEndpoint` and `DirectReadXferComplete`.

`usb_direct_read_cache_test.cpp` gives them stand-ins:
- a model of the Cortex-M7 data cache: write-back with 32-byte lines, filled on reads and writes of the processor. DMA stores go to
  RAM and not through the cache. `SCB_CleanInvalidateDCache_by_Addr` and `SCB_InvalidateDCache_by_Addr` step through the lines as in
  CMSIS 5.4.0 `core_cm7.h`. An invalidate of a dirty line is counted, because it loses data the processor wrote;
- TinyUSB: `usbd_edpt_xfer` cleans and invalidates the buffer as the SAMx7x driver does and records the transfer; the CDC FIFO is full,
  so `TakeReceiveEndpoint` can take the endpoint (LibTinyusb `src/tusb_config.h`: 512-byte FIFO at high speed);
- the memory: two 4096-byte buffers next to each other, as `GCodeInput` allocates them, with a line of other data before and after
  them that the processor has written (dirty in the cache).

Each run reads through the buffer first, as the reader did with the previous chunk, then starts a direct read, fills cache lines of
the buffer speculatively while the DMA stores, completes the transfer and reads the received bytes through the cache. Runs: both
buffers, reads of 512, 1024 and 4096 bytes, ended by the host after the whole length or after 300 bytes, and no speculative fill,
the first line, the line with the last received byte, the line after it, or every line. Each run checks that the reader gets the
bytes the DMA stored, that no dirty line was invalidated, that the data next to the buffers is unchanged, and that the completion
is reported once. Three more runs start a read at an offset of 16 or 4 bytes, or with a length of 500, and expect the assertion
(in a child process, because `vAssertCalled` does not return).

Without the fix the plain build fails 45 of 57 runs: the 42 runs with a speculative fill, where the reader gets the old content of the
filled lines, and the 3 runs that expect the assertion.

Not covered:
- whether and when the Cortex-M7 really fills these lines speculatively. The test only shows what happens if it does; the failure has
  not been seen on a board;
- the other direct transfers of SerialCDC (`readDirect`, `writeDirect`, used by the SBC over USB): their buffers are in the non-cached
  area;
- `GetDirectReadProgress`, which reads the DMA address register and not the buffer.

The Makefile reads CoreN2G from next to this repository, `../../../CoreN2G` (override with `COREN2G=`). The firmware builds do not use
this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/usb-direct-read-cache-test and build/asan-usb-direct-read-cache-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against CoreN2G without the fix: `make clean check COREN2G=<checkout of the previous commit>`. The generated
files depend on the source files, not on `COREN2G=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
