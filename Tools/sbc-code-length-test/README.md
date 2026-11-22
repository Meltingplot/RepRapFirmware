# SBC code length host test

This test runs RRF's real code that stores text and binary codes in a G-code buffer on a PC, and checks the two length limits in SBC
mode:
- **Text:** "The maximum length of a GCode command including the command itself and all parameters is 255 characters" (Duet3D wiki,
  `User_manual/Reference/Gcodes.md`). `MaxGCodeStringLength` (`src/Config/Configuration.h`) is 256 including the terminating null.
- **Binary:** DSF sends binary codes of up to `MaxGCodeBinaryLength` = 384 bytes (`src/SBC/SbcMessageFormats.h`, DSF
  `Settings.MaxCodeBufferSize`). The binary encoding is longer than the text, with a 20-byte `CodeHeader` and 8 bytes per parameter.
  `SbcInterface` refuses longer codes when they arrive.

## What is tested and why

A G-code buffer allocates its buffer once, when it gets its first code: `StringParser::Put` for a text code, `BinaryParser::Put` for a
binary code. In SBC mode one channel can get both:
- USB, Aux and the codes that RRF puts itself, for example a trigger action, are text.
- The macros that DSF runs on the same channel are binary.

So `StringParser::Put` allocates `MaxGCodeBinaryLength` bytes in SBC mode, "because binary codes have slightly more overhead". But
both parsers take their limit from the same `GCodeBuffer::bufferLength`:
- `StringParser::Put` records 256 for its 384 bytes, so a binary code of 257 to 384 bytes on that channel is refused as too long.
- `BinaryParser::Put` records 384, so a text line on a channel whose first code was binary may have up to 383 characters.

The test puts codes in both orders, in SBC mode and in standalone mode. It checks:
- that a text line of 255 characters is stored and one of 256 characters is refused;
- that a binary code of up to 384 bytes is stored and one of 388 bytes is refused;
- the size of the allocation.

It compiles the real text, which the Makefile extracts at build time with `../macro-error-test/extract.awk`, with `#line` directives
that point at the real lines (the build fails if one is missing):
- `src/GCodes/GCodeBuffer/StringParser.cpp`: `Put(char)`, `AddToChecksum` and `StoreAndAddToChecksum`.
- `src/GCodes/GCodeBuffer/BinaryParser.cpp`: `Put`.
- `src/Config/Configuration.h` and `src/SBC/SbcMessageFormats.h`: `MaxGCodeStringLength` and `MaxGCodeBinaryLength`.

`sbc_code_length_test.cpp` gives them minimal `GCodeBuffer`, `StringParser`, `BinaryParser`, `RepRap` and `Tasks::AllocPermanent`
stand-ins. `GCodeBuffer::overflowed` is what the firmware reports as "GCode command too long": `StringParser::CheckMetaCommand` for text,
`GCodes::ActOnCode` for binary.

Up to 3.7-dev de1bfe598, two cases fail:
- "first code text: binary codes up to MaxGCodeBinaryLength bytes are stored";
- "first code binary: text lines keep the limit".

## Running

```
make check
```
