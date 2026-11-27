# Queued code buffer host test

This test runs RRF's real `QueuedCode::AssignTo` from `src/GCodes/GCodeQueue.cpp` on a PC. The Makefile copies its text out of the
source at build time, as `Tools/sbc-code-buffer-alloc-test` does. `queued_code_buffer_test.cpp` gives it a minimal `QueuedCode` and a
stand-in `GCodeBuffer`.

## What is tested and why

Some codes of a file run in step with the moves before them. While a file is executed and the code has no expression
(`GCodeBuffer::CanQueueCodes`, `src/GCodes/GCodeBuffer/GCodeBuffer.h:521-524`), M3/M4/M5, M42, M104, M106, M107, M140 and some
others (`GCodeQueue::ShouldQueueMCode`, `src/GCodes/GCodeQueue.cpp:35-102`) and G10 that does not change tool offsets (no axis
letters, `GCodeQueue::ShouldQueueG10`, lines 27-32) are queued, but only while moves are in flight (lines 29 and 38) and only if the
code fits in a queue item of `BufferSizePerQueueItem` = `ShortGCodeLength` = 64 bytes (`src/GCodes/GCodeQueue.h:15`,
`src/Config/Configuration.h:172`). The callers are `src/GCodes/GCodes2.cpp:365-367` (G10) and `743-748` (M-codes).
`QueuedCode::AssignFrom` stores the code (`GCodeQueue.cpp:246-253`), and when its move is done `GCodeQueue::FillBuffer` (lines
140-158, from `src/GCodes/GCodes.cpp:744`) puts it into the G-code buffer of the Queue channel with `QueuedCode::AssignTo` (lines
255-274). In SBC mode the code is binary, and `AssignTo` hands it to `GCodeBuffer::PutBinary`
(`src/GCodes/GCodeBuffer/GCodeBuffer.cpp:322-332`) and on to `BinaryParser::Put` (`src/GCodes/GCodeBuffer/BinaryParser.cpp:33-76`).

Since upstream e520116e9 `BinaryParser::Put` no longer allocates the buffer of a G-code buffer. `SbcInterface::FillBuffer` calls
`GCodeBuffer::AllocateBinaryBuffer` (`GCodeBuffer.cpp:334-338`) when it finds the first code of a channel in the SBC code ring
(`src/SBC/SbcInterface.cpp:2301-2306` and `2381-2384`). The Queue channel gets its codes from `GCodeQueue::FillBuffer`, not from that
ring, so its G-code buffer had no buffer and a `bufferLength` of 0 (`GCodeBuffer.cpp:117`). `BinaryParser::Put` found every queued
code longer than that and set `overflowed` (`BinaryParser.cpp:43-48`), and `GCodes::ActOnCode` threw "GCode command too long"
(`GCodes2.cpp:125-128`). Upstream 187840328 ("Fixed queued codes failing with "GCode command too long" in SBC mode") makes
`AssignTo` allocate the buffer before `PutBinary` when the G-code buffer has none (`GCodeQueue.cpp:260-264`).

The stand-in `PutBinary` plays the length check and the copy of `BinaryParser::Put`: a code longer than `bufferLength` overflows and
is not stored (`BinaryParser.cpp:43-50`), and so does every code while there is no buffer. Otherwise the code is copied word by
word, as `memcpyu32` does (line 53), so UBSan sees a queue item that is not on a 4-byte boundary. The parameter check and the line
number and G53 flags (lines 55-75) are not played. The test checks:
- that a binary queued code put into a G-code buffer without a buffer gets a buffer before `PutBinary`, and that the code is stored
  whole without overflowing;
- that a G-code buffer that has a buffer is not allocated again. `Tasks::AllocPermanent` memory is never freed
  (`src/Platform/Tasks.cpp:321-326`), and `BinaryParser::Put` keeps the buffer it found first as `header` (`BinaryParser.cpp:37-40`);
- that consecutive queued codes in one G-code buffer share the buffer allocated for the first;
- that the fork's channel epoch 0 reaches `PutBinary`. The fork keeps the three-argument `PutBinary(data, len, 0)`, "A queued code has
  no channel epoch, so its replies are not tagged" (`GCodeQueue.cpp:266-267`); `PutBinary` sets `codeEpochValid` from
  `ChannelEpochValid` (`GCodeBuffer.cpp:326`, `src/SBC/SbcMessageFormats.h:115`);
- that a text queued code is put with `PutAndDecode` (`GCodeQueue.cpp:272`) and allocates no binary buffer. The string parser is not
  played; it allocates a buffer of its own in `StringParser::Put` (`src/GCodes/GCodeBuffer/StringParser.cpp:93-104`).

The epoch check starts with a buffer, so that it checks the epoch alone. The test builds with `HAS_SBC_INTERFACE` set to 1.

Built against the text of 3.7-dev d06ffdb5d, before the fix, two checks fail, both with "PutBinary was called without a buffer; the
code overflowed":
- "a binary queued code put into a G-code buffer without a buffer gets one before PutBinary and is stored whole";
- "consecutive binary queued codes in one G-code buffer share the buffer allocated for the first".

## Running

```
make check
```

To run it against another tree, for example one with the pre-fix `src/GCodes/GCodeQueue.cpp`:

```
make clean
make RRF=<tree> check
```
