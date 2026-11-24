# Binary G-code fuzz target

Host fuzz target for the binary G-codes that DSF sends to RRF in SBC mode, built with AddressSanitizer and UndefinedBehaviorSanitizer:
`BinaryParser`, from the moment a code is in the buffer of its G-code buffer (`GCodeBuffer::PutBinary` -> `BinaryParser::Put`) until
GCodes has read every parameter, printed the code and passed its parameters to a macro.

Why this target: it is the second step of the same trust boundary as `Tools/sbc-packet-fuzz`. `SbcInterface` accepts a Code packet of 20
to 384 bytes in a multiple of 4 (`SbcInterface.cpp:394-400`) and stores it as it is; everything inside the code (`numParameters`, the
`intValue` of each parameter, which is the length of a string or the count of an array) comes from DSF and is used by `BinaryParser`
without a check against the length of the code. `Tools/sbc-packet-fuzz` covers the way to `PutBinary`, this target the parser itself.

## What is real and what is played

Real, compiled unchanged:
- `src/GCodes/GCodeBuffer/BinaryParser.cpp`, every function. The build compiles a copy in `gen/` (`#line 1` plus the file as it is), so
  that its quoted includes `"GCodeBuffer.h"` and `"ExpressionParser.h"` find the stand-ins in `shim/` first
- `src/Platform/Heap.cpp` and `src/Platform/StringHandle.cpp`: the string heap that `AddParameters` stores string parameters in
- `StringRef`, `String<N>`, `SafeVsnprintf`, `Strnlen`, `StringFunctions` (with the real `SafeStrncpy`), `SafeStrtod` (`StrToU32`,
  `StrHexToU32`), `NumericConverter`, `IPAddress`, `NamedEnum` of RRFLibraries
- the headers `BinaryParser.h`, `SbcMessageFormats.h`, `GCodeException.h`, `StringHandle.h`, `Heap.h`, `GCodeChannel.h`, `TypeCode.h`,
  `Configuration.h`

Real text, copied out at build time with `../sbc-packet-fuzz/extract.awk`: `GCodeException::GetMessage` and the two `ThrowGCodeException`
(`gen/GCodeException.inc`), `struct DriverId` of `RepRapFirmware.h` and `GCodeBufferState` of `GCodeBuffer.h` (`gen/RrfTypes.inc`).

Played (`shim/`, `binary_code_fuzz.cpp`):
- `GCodeBuffer`: the members that `BinaryParser` uses, with the names of the real class. The code buffer is allocated once with
  `BinaryParser::AllocateBuffer` (384 bytes, `MaxGCodeBinaryLength`) and filled with byte 3 of the input before each code, as the rest of
  earlier codes; `Tasks::AllocPermanent` gives every allocation a heap block of its own, so that ASan sees a read beyond the 384 bytes
- `ExpressionParser`: it reads the first character of the expression the way the real parser does (`CurrentCharacter`,
  `ExpressionParser.cpp:2608`, returns `*currentp` only while `currentp < endp`) and returns a value chosen by it, see
  `shim/ExpressionParser.h`. So an expression whose start `BinaryParser` computed wrongly is read once; the real parser would go on
  reading, which this target does not model
- `ExpressionValue` (reference counts of heap strings as in the real class) and `VariableSet` (keeps a copy of each macro parameter);
  the constructors of `GCodeException` that take a G-code buffer (they only add the file and line of a file macro); the RTOS locks
- an invariant check: a heap string must end within its storage space (`Heap.h:22-26`). `StringHandle::InternalAssign` makes sure of
  that for every length that fits; a string that does not end there is read on into the next storage spaces of the 2 KB heap block,
  which ASan cannot see. The check reports `FUZZ: unterminated-heap-string`

## Input

Byte 0, what GCodes does with the code: bit 0 Gcodes debugging (DecodeCommand prints the code, `BinaryParser.cpp:82-86`), bit 1 the "Bad
command" reply (`GCodes2.cpp:166-167`; M122 prints the code of every busy channel the same way, `GCodeBuffer.cpp:242-255`), bit 2
`PrintCommand`, bit 3 `GetUnprecedentedString`, bit 4 `allowEmpty` for it, bit 5 `AddParameters` as for a macro call
(`GCodes.cpp:3718`) followed by reading every parameter back, bit 6 `codeRunning` 98 for it, bit 7 `ContainsExpression`,
`GetFilePosition`, `SetFilePosition`. Byte 1: the getter that GCodes calls after `Seen` for the first parameter letter (13 getters, each
further letter takes the next). Byte 2: the array length GCodes passes to the array getters (1 to 16; the array is exactly that long).
Byte 3: the fill byte of the code buffer. Then the code, 20 to 384 bytes in a multiple of 4; other lengths do not run, because
`SbcInterface` refuses them. The 325 seeds are codes as DSF's `Writer.WriteCode` encodes them (`Writer.cs`), with several configurations.

## Build and run (WSL, g++ 13, clang 18 for libFuzzer)

    make              # build/binary-code-fuzz and build/asan-binary-code-fuzz (deterministic driver, ../sbc-packet-fuzz/fuzz_driver.inc)
    make check        # the seeds, then 20000 mutated inputs whose reports must all be in findings/known.txt
    make findings     # every input in findings/ must still report the key on its "# expect:" line
    make coverage     # gcov line coverage of BinaryParser and StringHandle (../sbc-packet-fuzz/coverage.py)
    make libfuzzer    # build/libfuzzer-binary-code-fuzz
    FUZZ_POISON_STALE=1 build/asan-binary-code-fuzz ...    # also report reads of the code buffer beyond the code (rest of earlier codes)

## Findings

The inputs are in `findings/`, each with its report. All defect lines are upstream code (git blame: Christian Hammacher 2019, David
Crocker 2020-2025). None is reachable from a DSF that writes what `Writer.cs` writes (it sets `numParameters` to the parameter count and
`intValue` to the UTF-8 byte count or the array length, `Writer.cs:107, 171-236`); they are reached by a DSF or SBC process that writes
inconsistent values, by a corruption that the CRC32 of SPI misses, and on USB by any corruption that reaches RRF. B6 is reachable
from a correct DSF.

| ID | Defect | Report | Effect on the board |
|----|--------|--------|---------------------|
| B1 | `numParameters` is not checked against the code length: `DecodeCommand` (`BinaryParser.cpp:91-97`), `Seen`, `ContainsExpression`, `WriteParameters`, `AddParameters` read up to 255 parameter entries (2040 bytes) | heap-buffer-overflow at 97 | parameters made up from the heap after the 384-byte buffer: wrong data |
| B2 | the `intValue` of a parameter moves the value pointer of the ones after it unchecked: `Seen` and `AddParameters` add `intValue * 4` or `AddPadding(intValue)` (`BinaryParser.cpp:139-150`, 1030-1041); the getters then read there (502, 508, 536, 558, 576, 603, 660, 665, 249, ...) | SEGV / heap-buffer-overflow in the getters | a read at a wild address: fault unless it lands in memory, else wrong data |
| B3 | `WriteParameters` puts `char string[param->intValue + 1]` on the stack and copies `intValue` bytes into it (`BinaryParser.cpp:911-912`). It runs for the "Bad command" reply, for M122 while the code is executing, for Gcodes debugging and for the logging of G10 (`GCodes3.cpp:223`, `GCodes.cpp:4262`) | stack-overflow at 911, VLA bound not positive at 911, signed overflow at 911 (`intValue` 0x7FFFFFFF), unknown-crash / SEGV / heap-buffer-overflow at 912 | a large `intValue` moves the stack pointer below the main task stack (2410 words on the MB6HC, `Tasks.cpp:69`) and the memcpy writes there: memory corruption, then a fault. A negative one copies (size_t)`intValue` bytes |
| B4 | `AddParameters` passes the int32 `intValue` of a string as `size_t` length to `StringHandle` (`BinaryParser.cpp:995`). For -3 to -6, `sizeof(StorageSpace) + len + 1` wraps (`StringHandle.cpp:69`), `AllocateSpace` returns a space of length 0 (`Heap.cpp:298`), and `space->length - sizeof(StorageSpace)` gives 0xFFFFFFFE as the copy limit (`StringHandle.cpp:70`) | negative-size-param in strncpy at StringHandle.cpp:70 | `strncpy` pads with zeros up to 4 GB from inside the heap block: memory corruption, then a fault |
| B5 | the same with -1: the copy limit is 0, `SafeStrncpy` writes `dst[-1]` (the high byte of the length field) and the string gets neither its text nor a null (`StringHandle.cpp:70`, `StringFunctions.cpp:136-140`) | FUZZ: unterminated-heap-string | the macro reads the parameter on into the next heap strings: wrong data |
| B6 | RRFLibraries `SafeVsnprintf.cpp:368` negates `INT_MIN` when it prints a G-code number or integer parameter of -2147483648 with `%d` (`BinaryParser.cpp:742, 760, 763, 860`) | UBSan negation overflow at SafeVsnprintf.cpp:368 | undefined behaviour; the printed text was not checked on the board |
| B7 | array getters: `CheckArrayLength` limits the count to the caller's array, so nothing is written beyond it, but the elements are read from beyond the code (`GetArray`, `BinaryParser.cpp:775-829`; `GetDriverIdArray`, 597-605; `CheckArrayLength`, 832-838) | heap-buffer-overflow at 558 | wrong data |
| B8 | `GetIPAddress` / `GetMacAddress` parse with `StrToU32` / `StrHexToU32` first and check the end against `intValue` afterwards (`BinaryParser.cpp:388-392`, 441-445) | heap-buffer-overflow at 444 | reads digits beyond the code: wrong data or an error |

What was fuzzed (2026-11-24, RRF 3.7-dev 6c45e07a7): the seeds; seed 1 and seed 3, 50000 inputs each; seed 2, 50000 inputs with
`FUZZ_POISON_STALE=1` (also reports reads of the rest of earlier codes in the buffer, the same functions as above); libFuzzer 360 s with
3 workers (173440 inputs, 2652 crash artifacts: 2427 with the keys of B1-B8, the other 225 report nothing in a new g++ process). Line
coverage after seed 1 with 50000 inputs (`make coverage RUNS=50000`): `BinaryParser.cpp` 554 of 582 lines; not reached are `DataStart`
and `DataLength` (the target does not call them), `ConstructParseException(..., const char *)` and single lines in the getters.
`StringHandle::InternalAssign`, `Delete`, `IncreaseRefCount` and `Get` 100 %.

Not found: no write beyond the array of an array getter, no write beyond a `String<N>` (`StringRef::copy` limits both), no report in
`Heap.cpp` itself. Observation without a sanitizer report: `WriteParameters` steps over the values of `BoolArray`, `ULong`, `Bitmap64`
and `DateTime` parameters, `Seen` and `AddParameters` do not, so a code with one of them before a string or array finds the wrong value;
DSF does not send these types (`Writer.cs:175-236`).
