# SBC resend reserve host test

This test runs RRF's real `src/SBC/DataTransfer.cpp` and `src/Storage/CRC32.cpp`, compiled unchanged, on a PC. It uses the shims
of `Tools/sbc-harness/shim` through the include path, and `resend_reserve_test.cpp` defines the few board functions that they
declare. The test checks the room that `DataTransfer` keeps in the transmit buffer for resend requests.

How that room works:
- `FreeTxSpace` (`DataTransfer.h`) is the transmit buffer minus `txPointer` (rounded up to 4) minus 8 bytes per packet received
  in this transfer (`GetRxNumPackets`, from `rxHeader` on SPI and `usbRxHeader` on USB). Every Write function except
  `ResendPacket` writes only what fits into it (`CanWritePacket`, and `WriteCodeReply` and `WriteFileData` cut their data to it).
- `ResendPacket` writes an 8-byte packet header without a check. `SbcInterface::ExchangeData` calls it at the end of its packet
  loop for every packet that it could not process (`packetAcknowledged` false: no room in the code buffer, an object model or
  evaluation result that does not fit, a channel whose abort is recorded, ...). The loop runs once per received packet, so there
  are at most as many resend requests as received packets, and they always fit into the room.
- A resend request goes into the room once less than 8 bytes are left before it. That happens when an answer in the same loop,
  for example an object model or an evaluation result, left 0 or 4 bytes. `FreeTxSpace` then has to report 0. Before the fix it
  went below 0 and wrapped round to almost the largest `size_t` (2^32 on the board, 2^64 here), so every later write of the
  transfer passed `CanWritePacket`: the next packets of the loop, the queued code replies (`WriteQueuedCodeReplies`), the code
  buffer update, file requests and file data, and the channel requests at the end of `ExchangeData`. `WriteCodeReply` then
  copied the whole reply past the end of the buffer.

Only the SBC task writes into the transmit buffer: `SbcInterface::TaskLoop` calls `ExchangeData` and then `StartNextTransfer`,
and every `transfer.Write...` and `ResendPacket` call in `SbcInterface.cpp` is made from `ExchangeData` or from
`WriteQueuedCodeReplies`, which `ExchangeData` calls. Other tasks only queue work for it (`gcodeReply`, `fileOperation`). So the
order of the writes within one call of `ExchangeData` is all that matters, and the test runs them in one thread.

The test sets the number of received packets, fills the buffer with one real EvaluationResult packet (`WriteEvaluationError`, as
an EvaluateExpression packet gets it) so that N bytes are left before the room, writes resend requests for the first packets,
then a code reply, an object model or file data, then the resend requests of the remaining packets. After every step it checks:
- the bytes left after `txPointer` still hold a resend request for every received packet that has none yet;
- `FreeTxSpace` reports no more than the bytes before the room that those resend requests need;
- every resend request and the packet written are correct, and the packet is written when it fits after the resend requests.

The checks hold whether `FreeTxSpace` keeps the room for all received packets or only for those without a resend request yet,
so a different fix of the same bug can be checked against them. The AddressSanitizer build also reports the first byte written
past the buffer, because every buffer is a heap block of its own here (on the board the buffers lie next to each other in the
PBUF pool). Each case runs in a child process, so a case that AddressSanitizer stops fails alone.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/sbc-resend-reserve-test and build/asan-sbc-resend-reserve-test (AddressSanitizer + UBSan), then run both

Both are built with `SUPPORTS_SBC_OVER_USB=1`, as the MB6HC release (see `Tools/sbc-harness/README.md`); one case switches the
transport to USB. Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails. The `-fpermissive`
warnings about pointer casts to `uint32_t` are expected, as in `Tools/sbc-harness`.
