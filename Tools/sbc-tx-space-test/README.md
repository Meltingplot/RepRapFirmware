# SBC transmit space host test

This test runs RRF's real `src/SBC/DataTransfer.cpp` and `src/Storage/CRC32.cpp`, compiled unchanged, on a PC. It uses the shims
of `Tools/sbc-harness/shim` through the include path, and `tx_space_test.cpp` defines the few board functions that they declare.
The test checks that `DataTransfer::WriteFileData` never writes past the transmit space that `FreeTxSpace` reports.

Why the end of the buffer matters:
- In SBC mode `FileStore::Write` sends file data through `SbcInterface::WriteFile` (`FileStore.cpp`, `Store`) and
  `SbcInterface::WriteFileAsync` (`StoreFullWriteBuffer`). The SBC task then calls `WriteFileData` in `ExchangeData`
  (`FileOperation::write` and `writeAsync` in `SbcInterface.cpp`).
- That happens after the packets of the transfer have been answered, the queued code replies have been written
  (`WriteQueuedCodeReplies`) and the code buffer update has been sent. So the space left for the file data depends on all of
  them. It is a multiple of 4: `SbcTransferBufferSize` is one (`SbcMessageFormats.h`), and `FreeTxSpace` rounds
  `txPointer` up to 4 and keeps 8 bytes per received packet for a resend request (`DataTransfer.h`).
- `WriteFileData` sends what fits and leaves the rest in `length`. When it returns false, `fileOperationPending` stays set and
  the SBC task tries again in the next transfer. When it returns true, the SBC task waits for `FileWriteResult` and sends the
  rest after it.

A WriteFile packet is a `PacketHeader` (8 bytes), a `FileHandleHeader` (4 bytes) and the data. The test fills the buffer with
one real `WriteCheckFileExists` packet (none when the whole buffer is free) so that exactly N bytes are free, calls
`WriteFileData` and checks:
- with 12 bytes or fewer free: it returns false, leaves `length` and `txPointer` alone and writes no packet;
- with more: it writes `min(length, N - 12)` bytes in one correct WriteFile packet and reduces `length` by that;
- in every case `txPointer` stays within the free space. With no received packets that is the end of the buffer, and the
  AddressSanitizer build also reports the first byte written past it, because every buffer is a heap block of its own here (on
  the board the buffers lie next to each other in the PBUF pool). With three received packets a write past the free space stays
  in the buffer, in the room kept for resend requests, so only the `txPointer` check sees it.

Each case runs in a child process, so a case that AddressSanitizer stops fails alone.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/sbc-tx-space-test and build/asan-sbc-tx-space-test (AddressSanitizer + UBSan), then run both

Both are built with `SUPPORTS_SBC_OVER_USB=1`, as the MB6HC release (see `Tools/sbc-harness/README.md`); the test stays on SPI.
Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails. The `-fpermissive` warnings about
pointer casts to `uint32_t` are expected, as in `Tools/sbc-harness`.
