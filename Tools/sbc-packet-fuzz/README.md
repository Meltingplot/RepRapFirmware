# SBC packet fuzz target

Host fuzz target for the packets that DSF sends to RRF in SBC mode, built with AddressSanitizer and UndefinedBehaviorSanitizer. It
looks for out-of-bounds reads and writes, wild pointers and undefined behaviour in the code that takes a transfer apart: the first
trust boundary of the SBC link, where every length and count comes from the other side.

Why this target first: a transfer is up to 8 KB that RRF reads with lengths taken from the data itself (`PacketHeader::length`, the
`*Length` and count fields of the data headers). The SPI transport checks a CRC32 of the header and of the data
(`DataTransfer.cpp:1099-1100`, `1187-1188`), the USB transport only the total length (`DataTransfer.cpp:1329-1331`, no CRC on the RRF
side). Neither checks that the lengths inside a packet agree with the packet or the transfer. The second target,
`Tools/binary-code-fuzz`, takes the G-code packets further, through `BinaryParser`. CAN messages from expansion boards and the G-code text
parser (`StringParser`) are not fuzzed (see the class 3 report).

## What is real and what is played

Real, compiled unchanged:
- `src/SBC/DataTransfer.cpp`: `ReadPacket`, `ReadData`, every `Read*` function, the `Write*` functions of the replies, `DoTransferUsb`,
  `SwitchToUsb`
- `src/Storage/CRC32.cpp`, `src/GCodes/GCodeFileInfo.cpp`, and `StringRef`, `String<N>`, `SafeVsnprintf`, `Strnlen`, `StringFunctions`,
  `SimpleMath` of RRFLibraries

Real text, copied out of the sources at build time with `extract.awk` (`#line` directives keep the real file and line numbers):
- the packet loop of `SbcInterface::ExchangeData` (`for (size_t i = 0; i < transfer.PacketsToRead() ...`, `gen/SbcPacketLoop.inc`),
  `SbcInterface::IsDroppedByEpoch`, `RecordChannelAbort`, `InvalidateBufferedCodes`, `FillBuffer` (`gen/SbcFunctions.inc`)
- `OutputBuffer::cat(const char *, size_t)`, `copy(const char *, size_t)`, `Clear`, `Length` of `src/Platform/OutputMemory.cpp`

Played (`shim/FuzzShim.h`, `sbc_packet_fuzz.cpp`):
- the SPI hardware and DMA: register writes only. The target hands `DataTransfer` a transfer whose CRCs have passed: it sets `rxHeader`
  and copies the data into `rxBuffer`, as `DoTransfer` leaves them, and sets the state to `ProcessingData`
- the USB device: `SerialCDC::readDirect` serves the bytes of the input, as DSF writes them
- the PBUF pool: each buffer is a heap block of its own of exactly the requested size, so that ASan sees a read beyond one of the 8 KB
  buffers. On the board `rxBuffer`, `txBuffer` and `lastTxBuffer` are next to each other in the PBUF pool in non-cached RAM
  (`LwipEthernetInterface.cpp:106-118`, `same70q20b_flash.ld:55`)
- the G-code buffers (state from byte 1 of the input; `PutBinary` copies the code as `BinaryParser::Put` does), `GCodes`, `PrintMonitor`,
  the variables, the expression parser, the object model, `SoftwareReset` and the IAP start (both end the input), the RTOS objects

## Input

Byte 0: bit 0 USB instead of SPI, bits 1-3 the file operation that waits for its result (none, read, getFileList, write, checkFileExists,
openRead, writeAsync, seek), bit 7 the length of an async write (16 bytes). Byte 1: the state of every G-code buffer (`GbFlags`). Byte 2:
the size of the read buffer of the file operation, an index into 0, 1, 7, 16, 64, 255, 256, 1024, 2048, 4096, 8191, 8192. Then the
transfers. SPI: per transfer one byte numPackets and a 16-bit dataLength (little endian), then dataLength bytes of data. USB: the bytes
that DSF writes, an 8-byte `UsbTransferHeader` and its data, as many transfers as the input holds. After each transfer the packet loop
runs (the SBC task), then `FillBuffer` for every channel (the main task). Inputs in `.hex` files are hex digits with `#` comment lines.

The 93 seeds (`FuzzSeeds`) are well-formed transfers with every request of `SbcRequest` on SPI and on USB, several packets per transfer,
and two transfers in one input.

The firmware builds do not use this folder: make and CMake only read `src/`, and the `*-fuzz` resource filter in `.project` hides it
from Eclipse.

## Build and run (WSL, g++ 13, clang 18 for libFuzzer)

    make              # build/sbc-packet-fuzz and build/asan-sbc-packet-fuzz (deterministic driver, fuzz_driver.inc)
    make check        # the seeds, then 20000 mutated inputs whose reports must all be in findings/known.txt
    make findings     # every input in findings/ must still report the key on its "# expect:" line (run-findings.sh)
    make coverage     # gcov line coverage per function and per case of the packet loop (coverage.py)
    make libfuzzer    # build/libfuzzer-sbc-packet-fuzz

    build/asan-sbc-packet-fuzz -seed 1 -runs 50000 -out <dir>     # mutation run, one .hex with the report per distinct report
    build/asan-sbc-packet-fuzz -classify <file>...                # the report key of each input, e.g. for libFuzzer artifacts
    FUZZ_POISON_STALE=1 build/asan-sbc-packet-fuzz ...            # also report reads of rxBuffer beyond the received data

Every run is bounded in memory (`ASAN_OPTIONS=hard_rss_limit_mb=2048`, see the Makefile). The deterministic driver runs the inputs in a
child process and starts a new one after an input that ended it, so one run lists every distinct report with the first input that
caused it. A report key is the kind of error and the first frame in `DataTransfer.cpp`, `SbcInterface.cpp` or `OutputMemory.cpp`.
UBSan's check of bool loads is off by default (`UBSAN_BOOL=1` turns it on): every `Read*` function with a bool field loads a packet byte
as bool (finding P7). The alignment check is off: `rxPointer` is always a multiple of 4, which is all the Cortex-M7 needs.

## Findings

The inputs are in `findings/`, each with its report (`# expect: clean` for a fixed one: the input no longer reports anything). All
defect lines are upstream code (git blame: Christian Hammacher, David
Crocker). None of them is reachable from a DSF that writes what `src/DuetControlServer/Link/Protocol/Writer.cs` writes; they are reached
by a DSF or SBC process that writes inconsistent lengths, by a corruption that the CRC32 of SPI misses, and on USB by any corruption
that reaches RRF, since RRF checks no CRC there. The common cause: `ReadPacket` checks that the 8-byte header fits in the transfer
(`DataTransfer.cpp:618`) and nothing checks `packet->length` or the lengths inside the data against what the transfer holds.

P1 to P5 are fixed:
- `ReadPacket` refuses a packet whose data doesn't fit in the rest of the transfer (P5), the way it refuses a header that doesn't fit:
  it returns no packet, and `ExchangeData` stops reading the transfer. DSF writes the data of each packet right after its header
  (`WritePacket` and `GetWriteBuffer`, `SPI.cs` and `USB.cs`), so every packet that DSF writes fits.
- `ReadGetObjectModel`, `ReadPrintStartedInfo`, `ReadMessage` and `ReadFileData` read their inner lengths only as far as they lie within
  the packet (P1-P4). That is the way `ReadFileData` already treats the read buffer and `StringRef::copy` its destination: a value
  that does not fit is cut. `ReadMessage` and `ReadFileData` get the packet length for that, as the other two already did.
- `ReadFileList` treats entry data beyond the packet as it treats entry data beyond the read buffer: "the two sides disagree about the
  format", end of list. Same defect as P3; the run with `FUZZ_POISON_STALE=1` showed it (use-after-poison in `ReadFileList`).

The table keeps the line numbers of RRF 3.7-dev 6c45e07a7, before the fix.

| ID | Request | Defect | Report | Effect on the board |
|----|---------|--------|--------|---------------------|
| P1 | GetObjectModel | `DataTransfer.cpp:655-659`: `keyLength` and `flagsLength` are not checked against the packet; `data += keyLength` | heap-buffer-overflow / SEGV at 659 | reads up to 64 KB beyond `rxBuffer`: wrong key and flags |
| P2 | SetPrintFileInfo | `DataTransfer.cpp:681-689`: `data += numFilaments * sizeof(float)` with a 32-bit count, then `filenameLength`, `generatedByLength` unchecked | SEGV at 685, heap-buffer-overflow at 689 | a wild address up to 16 GB away (mod 2^32 on the board): fault unless it lands in memory |
| P3 | FileReadResult | `DataTransfer.cpp:775-781`: `bytesRead` is checked against the read buffer, not against the packet | heap-buffer-overflow at 781 | copies up to the buffer size from beyond the packet into the file data: wrong data |
| P4 | Message | `DataTransfer.cpp:731-732`: `header->length` unchecked, `OutputBuffer::cat` copies it (`OutputMemory.cpp:240`) | heap-buffer-overflow at OutputMemory.cpp:240 | message text from beyond the packet |
| P5 | WriteIap | `SbcInterface.cpp:749`: `ReceiveAndStartIap(transfer.ReadData(packet->length), packet->length)` | heap-buffer-overflow at 749 | the IAP image gets bytes from beyond the packet; DSF can flash anything here anyway |
| P6 | every request with a data header | the `Read*` functions read their header and data beyond the received data but inside `rxBuffer`. Open after the fix of P1-P5: `ReadDataHeader` reads the whole header of a packet that is shorter than it; `ReadSetVariable` and `ReadDeleteLocalVariable` read their 8-bit lengths unchecked; `ObjectModelKeyChanged` compares the key as a null-terminated string (`SbcInterface.cpp:1455`) | use-after-poison with `FUZZ_POISON_STALE=1` | stale bytes of an earlier transfer: wrong data, no memory error |
| P7 | every request with a bool field | `ReadBoolean` (`DataTransfer.cpp:645`) and the others load a packet byte other than 0 or 1 as bool | UBSan load of invalid bool (`UBSAN_BOOL=1`) | undefined behaviour; what the firmware build makes of such a byte was not checked |

After the fix of P1-P5 (2026-11-24): the seeds; seed 1 and seed 3, 50000 inputs each, report nothing. Seed 2 with 50000 inputs and
`FUZZ_POISON_STALE=1` reports P6 only. With only the `ReadPacket` check and none of the others, P1-P4 still report and P5 does not.

What was fuzzed (2026-11-24, RRF 3.7-dev 6c45e07a7): the seeds; seed 1 and seed 3, 50000 inputs each; seed 2, 50000 inputs with
`FUZZ_POISON_STALE=1`; libFuzzer 600 s with 4 workers (2.06 million inputs, 5589 crash artifacts: 5454 with the keys above, the other 135
report nothing in a new g++ process; the 20 of them that the libFuzzer build reproduces on its own are P1, P2 and P4, in part with the
frame of `ReadGetObjectModel` dropped by a tail call).

Line coverage after seed 1 with 50000 inputs (`make coverage RUNS=50000`): the read side of `DataTransfer.cpp` (`ReadPacket` to
`ReadSetLastCodeResult`) 107 of 109 lines, `SwitchToUsb` 10/10, `DoTransferUsb` 18/21; `DataTransfer.cpp` as a whole 337/914, the rest
being the SPI and DMA code, diagnostics and the `Write*` functions of requests that the packet loop does not answer. The extracted
`SbcInterface` functions 112/127, each of the 29 cases of the packet loop at least 57 % (`FileTruncateResult`), 23 of them above 85 %.
