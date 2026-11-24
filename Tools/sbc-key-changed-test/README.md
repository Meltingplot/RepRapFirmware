# ObjectModelKeyChanged host test

This test checks how RRF reads the key of an `ObjectModelKeyChanged` request from the SBC.

## Why

DSF provides the object model keys `network` and `volumes` itself in SBC mode. When one of them changes, it sends `ObjectModelKeyChanged`
with the key (DSF `Model/PeriodicUpdateService.cs`, `ObjectModelKeyChanged("network")` and `("volumes")`). The packet is a string
request: a `StringHeader` with the length of the key, then the UTF-8 key, padded with zeros to a multiple of 4 (DSF
`Link/Adapter/SPI.cs` `WriteObjectModelKeyChanged`, `Link/Protocol/Writer.cs` `WriteStringRequest`; the USB adapter writes the same).

The case in `SbcInterface::ExchangeData` (`src/SBC/SbcInterface.cpp`) took the start of the packet data as the key:

    const char *key = transfer.ReadData(packet->length);
    if (StringEqualsIgnoreCase(key, "network")) ...

So it compared the `StringHeader` with "network" and "volumes", never matched, and `RepRap::NetworkUpdated` and `VolumesUpdated`
(`src/Platform/RepRap.h`) never ran for these requests. The sequence numbers `seqs.network` and `seqs.volumes` that RRF reports
(`src/Platform/RepRap.cpp`) did not change when DSF changed these keys. Which clients react to these two sequence numbers in SBC mode was
not followed.

The fix reads the packet the way `DataTransfer` reads the other strings, with the length from their header (`ReadGetObjectModel`,
`ReadPrintStartedInfo`): `DataTransfer::ReadObjectModelKeyChanged` reads the `StringHeader` and copies the key with its length into a
`String<StringLength20>`, as far as it lies within the packet (the limit that RRF #99 adds to the other reads). Behaviour change: the two
sequence numbers now increment when DSF reports a change of `network` or `volumes`.

Upstream code (the request came with SBC protocol version 7, d3c4957e4). The same case is in Duet3D 3.7-dev and 3.7-ch. Found by the
review of the SBC link fixes (finding RV-1, error class 3, 2026-11-24). Not seen on a board.

## What the test compiles

The real text of the case `SbcRequest::ObjectModelKeyChanged` of `SbcInterface::ExchangeData`, of `DataTransfer::ReadData`,
`ReadDataHeader` and `ReadObjectModelKeyChanged` (`src/SBC/DataTransfer.cpp`) and of `DataTransfer::AddPadding`
(`src/SBC/DataTransfer.h`), which the Makefile extracts with `../macro-error-test/extract.awk` (with `#line` directives that point at
the real lines). RRFLibraries `String`, `StringRef` and `StringFunctions` are the real ones. `sbc_key_changed_test.cpp` has the
`PacketHeader`, `StringHeader` and `SbcRequest` of `src/SBC/SbcMessageFormats.h`, a `RepRap` that counts the two sequence numbers, and a
`DataTransfer` that holds each transfer in a heap block of exactly its size, so that AddressSanitizer sees a read beyond it.

The test builds the packets as DSF writes them. Eight cases: "network", "volumes", both in one transfer (each case must read exactly its
packet), "NETWORK" (the compare ignores case, as before), "move" (no sequence; its length is a multiple of 4, so no zero follows it), a
key longer than 20 characters, a `StringHeader` length shorter than the key, and one beyond the packet at the end of the transfer.

Without the fix the first four fail (the sequence numbers stay 0). Without the limit to the packet, the AddressSanitizer build stops at
the last case with a heap-buffer-overflow in `Strnlen`, called from `ReadObjectModelKeyChanged`. With the fix all 8 pass.

Not covered: a packet shorter than its `StringHeader` (finding FZ-P6, open), and the DSF side.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

To see the failure, run it against the source without the fix, which has no `ReadObjectModelKeyChanged`:

    make clean check RRF=<checkout of the previous commit> TRANSFER_FUNCTIONS="DataTransfer::ReadData DataTransfer::ReadDataHeader"

The Makefile reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The firmware builds
do not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
