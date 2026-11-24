# SBC variable answer host test

This test checks what RRF does with a `SetVariable` or `EvaluateExpression` request from the SBC whose answer does not fit into a transfer.

## Why

On the lab printer (MB6HC, 3.7.0+1.mp.9, SBC over SPI) the line

    global m = vector(128,vector(128,vector(30,10)))

was reported as a crash of RRF, while the same line with 20 in place of 30 was not. The object model of DSF stopped for both, which
`../sbc-om-overflow-test` covers. The difference between the two comes from the answer to the SetVariable request. DSF sends `global`, `var` and `set` to RRF as a SetVariable
request (DSF `Codes/Handlers/KeywordHandler.cs`, `SetVariableAsync`), and RRF answers with the new value, as JSON for an array
(`SbcInterface.cpp`, case `SetVariable`). DSF only logs that value (`KeywordHandler.cs`, "Set variable {Variable} to {Value}").

The answer of an array is a root array, which `ReportHeapArrayAsJson` cuts after the element that crosses half the buffer space. Here that
is the first element, `vector(128, vector(n, 10))`, 128 * (3n + 1) + 129 bytes: 7937 for n = 20 and 8321 for n = 21. A transfer has 8192
bytes, less 8 for the resend request of every packet the SBC sent in it (`DataTransfer::FreeTxSpace`) and 8 for the packet header of the
answer, so the answer may have 8176 bytes of data, the `EvaluationResultHeader` (8) and the variable name included. For n >= 21 the
answer never fits.

The case assigned the value first and wrote the answer afterwards. When the answer did not fit, it did not acknowledge the request, so DSF
sent it again, and:
- `global` and `var` created the variable the first time and answered "variable 'global.m' already exists" the second time.
- `set` assigned the value again on every attempt and never answered, so the code channel waited for ever.
- The same happened to every value, also a plain one, when the rest of the transfer had no room for its answer: `set var.i = var.i + 1`
  added 1 once for each attempt.

The case `EvaluateExpression` (an expression that DSF sends to RRF to evaluate) did not acknowledge an array value whose
JSON is longer than a transfer either, so DSF sent it again for ever, and RRF built the JSON each time.

The fix:
1. SetVariable makes the answer before it assigns the value. A value whose answer can never fit into a transfer is refused with "value too
   long to send to the SBC", and a value whose answer does not fit into the rest of this transfer is not assigned, so that the request
   can come again (`DataTransfer::CanWriteSetVariableResult`). The value is assigned only together with its answer, once.
2. EvaluateExpression answers an array value that can never fit with the same error.
3. Both wait until 36 output buffers are free before they write an array as JSON (`MinimumBuffersForSbcJson`, as for GetObjectModel), so
   that JSON that overflows the buffers is surely longer than a transfer, and the error is never caused by output that holds buffers.
   `SbcInterface::MakeJsonAnswer` writes the JSON for both cases, in place of the four copies before.

Behaviour change: in SBC mode a `global`, `var` or `set` whose answer is longer than a transfer fails with "value too long to send to the
SBC" and leaves the variable as it was; RRF without an SBC still takes it. An expression whose array value is too long gets the same error.
An array value waits while fewer than 36 buffers are free. A value of an object model array is written as JSON before it is stored now, so
one whose JSON is too long gets "value too long to send to the SBC" where the error of `Variable::MakeStorable` came before (both refuse the
value). DSF refuses a literal value of this kind before it sends it (DSF PR #239, same text).

Upstream code: the two cases are the same in Duet3D 3.7-dev. Found on the lab printer on 2026-11-24.

## What the test compiles

The real text of the cases `SbcRequest::EvaluateExpression` and `SbcRequest::SetVariable` of `SbcInterface::ExchangeData`, of
`SbcInterface::MakeJsonAnswer` and of the constants of the fix (`src/SBC/SbcInterface.cpp`), of the `DataTransfer` functions that write
the answers into the transmit buffer and check their room (`src/SBC/DataTransfer.cpp`: `WriteSetVariableResult`, `CanWriteSetVariableResult`,
`GetSetVariableResultLength`, `WriteEvaluationResult`, `WritePacketHeader`, ...; `src/SBC/DataTransfer.h`: `FreeTxSpace`, `CanWritePacket`, ...),
of `ObjectModel::ReportHeapArrayAsJson` (`src/ObjectModel/ObjectModel.cpp`) and of the `OutputBuffer` functions they use
(`src/Platform/OutputMemory.cpp`), which the Makefile extracts with `../macro-error-test/extract.awk` (with `#line` directives that point at the
real lines). The output buffers are the real pool of 40 buffers of 256 bytes with 4 reserved, and the transmit buffer has the 8192 bytes of a
transfer. `sbc_variable_answer_test.cpp` has the class declarations and stand-ins: a parser for integers, `vector(n, value)`, `var.name`,
`global.name`, a sum and `omarray(n)` (an object model array with n characters of JSON), variable sets, and a request that the transfer
returns from `ReadSetVariable` and `ReadEvaluateExpression`. Arrays hold their elements by reference, as in RRF.

The cases:
- `global` with the values of the lab printer for n = 20 (assigned, answered with the first element), 21, 30 and 60 (refused); `var` and
  `set` with n = 30 (refused, old value kept).
- An answer of 8176 bytes, the longest that fits (sent), and one of 8177 bytes (refused).
- Object model arrays of 100, 9000 and 20000 characters.
- `set var.i = var.i + 1`, `global n = 5` and `global a = vector(3, 10)` while the rest of the transfer has no room for the answer (wait,
  unchanged), then sent again (assigned once).
- 35 and 36 free buffers, for an array and a plain value.
- An array index out of bounds after the answer is made (error, answer released).
- Expressions: a small and a too long array, a too long object model array, an answer that does not fit into the rest of the transfer,
  and a plain value.

Every case also checks that all buffers are free again afterwards and that nothing is written past the transmit buffer.

Without the fix 19 of the 28 fail: the too long values are assigned and not acknowledged, `var.i` becomes 2 and then 3, `global n` and
`global a` sent again fail with "variable ... already exists", the two cases with 35 free buffers do not wait, and the too long expressions
are not acknowledged. With the fix all 28 pass.

Not covered: the board itself, the real expression parser and `Variable::MakeStorable`, and how DSF handles the error.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both
    make clean check RRF=<checkout without the fix> RRFLIBS=<RRFLibraries> WITHOUT_FIX=1
