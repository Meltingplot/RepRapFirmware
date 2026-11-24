# Expression parser fuzz target

Host fuzz target for the RRF expression parser, built with AddressSanitizer and UndefinedBehaviorSanitizer: `ExpressionParser` from
the text of an expression to its value, with the object model, variables, the string and array heap and `SafeStrptime` behind it.

Why this target: an expression is user input from every channel (DWC and DSF, macros, job files, USB, PanelDue: `echo {...}`, `if`,
`var`, `set`, every parameter in `{...}`), and the parser walks the object model, allocates in the heap and recurses. It is the next
trust boundary behind `Tools/sbc-packet-fuzz` and `Tools/binary-code-fuzz`.

## What is real and what is played

Real, compiled unchanged (with `-finstrument-functions` for the stack model):
- `src/GCodes/GCodeBuffer/ExpressionParser.cpp`, every function. The build compiles a copy in `gen/` (`#line 1` plus the file as it
  is), so that its quoted include `"GCodeBuffer.h"` finds the stand-in in `shim/` first
- `src/ObjectModel/ObjectModel.cpp` (copy in `gen/ObjectModel/` next to the copy of `ObjectModel.h`, see below), `Variable.cpp`
- `src/Platform/Heap.cpp`, `StringHandle.cpp`, `ArrayHandle.cpp`: the string and array heap
- `src/libc/strptime.cpp` (`SafeStrptime`, used by `datetime()`)
- RRFLibraries `StringRef`, `SafeVsnprintf`, `Strnlen`, `StringFunctions`, `SafeStrtod`, `NumericConverter`, `IPAddress`, `NamedEnum`,
  `IP4String`
- the headers `ExpressionParser.h`, `Variable.h`, `GCodeException.h`, `Heap.h`, `StringHandle.h`, `ArrayHandle.h`, `TypeCode.h`,
  `Configuration.h`, and the object model macros of `ObjectModel.h` (the object model tables of the target are built with them)

Real text, copied out at build time with `../sbc-packet-fuzz/extract.awk`: `GCodeException::GetMessage` and the two
`ThrowGCodeException` (`gen/GCodeException.inc`), `struct DriverId` (`gen/RrfTypes.inc`), `GetFloatFormatString` of `RepRapFirmware.cpp`
(`gen/RrfFunctions.inc`).

Changed for the host, nothing else:
- `ObjectModel.h`: `uint32_t whole` of the `ExpressionValue` union becomes `uintptr_t whole` (`gen/ObjectModel/ObjectModel.h`), because
  the union holds pointers, which are 8 bytes on the host. The Makefile checks that the line changed exactly once
- `-funsigned-char` (char is unsigned on ARM), `-fpermissive` (Heap.cpp casts a pointer to `uint32_t`)
- a `NamedEnumLookup(unsigned int)` bridge in `expression_fuzz.cpp`: on the 64-bit host `size_t` and `unsigned int` are two overloads
- libFuzzer build only (clang): copies of `Heap.cpp` (cast through `uintptr_t`), `Configuration.h` and `ExpressionParser.cpp` (each
  without one `static_assert` on `strlen` that clang does not evaluate), `ArrayHandle.cpp` (`elements[]` declared `elements[0]`, same
  layout), and the ordering check of the object model tables switched off. The g++ builds use the files as they are

Played (`shim/`, `expression_fuzz.cpp`):
- the object model: a `RepRap` root with 12 tables and 12 array entries built with the real `ObjectModel.h` macros. They cover every
  `TypeCode` that a value can have (bool, char, float, int32 incl. `job.layer` = 2147483647, uint32, uint64, strings, date-time,
  IP and MAC addresses, driver IDs, bitmaps, special, null, objects, empty and null arrays) and nested arrays whose functions use the
  parent index the way the real ones do: `move.axes[].drivers` with `context.GetLastIndex()` and `context.GetIndex(1)` as
  `Move.cpp:104-105`, `move.axes[].workplaceOffsets` with `GetIndex(1)` as `Move.cpp:112`. `state.names` is guarded by
  `reprap.globalsLock` as the real global variables are
- global variables (scalars, strings, nested arrays incl. an empty one), `job.file.customInfo`, local variables and macro parameters of
  the G-code buffer, `iterations` 2, `line` 42, `result`; the G-code buffer is `M118`, not in a file
- files for `fileread()` and `fileexists()`: 7 small files in memory; the target reports a read after close, a double close and a file
  left open (`file-read-after-close`, `file-double-close`, `file-leak`)
- `ReadWriteLock` with the semantics of the real single-task case (`RTOSIface.cpp:235-448`), reporting `deadlock` when a task asks for a
  write lock while it holds a read lock of the same lock; after every input the heap lock must be free (`heap-lock-leak`)
- `REPORT_INTERNAL_ERROR`, `THROW_INTERNAL_ERROR` and `RRF_ASSERT` report `internal-error <function>` / `RRF_ASSERT`. On the board they
  print a message and go on; here they stop the run so that the fuzzer sees them
- `debugPrintf`, `Platform::MessageF` (printed with `-v` only), `millis64`, `random32` (a fixed LCG), the GCodeException constructors that
  take a G-code buffer

## Stack model

The real `ExpressionParser::CheckStack` (`ExpressionParser.cpp:2661`) and `ObjectExplorationContext::CheckStack` (`ObjectModel.cpp:637`)
compare the stack pointer with the stack limit of the task. Host frames say nothing about the board, so `board_stack.cpp` keeps a
virtual board stack: every RRF and RRFLibraries function is compiled with `-finstrument-functions`, and on entry the hook adds the frame
that the same function has in the MB6HC firmware, on exit it takes it off again. `GetStackPointer()` and
`TaskBase::GetCurrentTaskStackBase()` return the virtual stack pointer and limit, so the real `CheckStack` code decides with board figures.

- The board frames are in `board-stack-usage.tsv` (4462 functions): the largest CFA offset of each function in the DWARF `.debug_frame`
  of `Duet3Firmware_MB6HC.elf`, which is the "static" figure of `-fstack-usage` (`board-frames.py`; `make stack-table ELF=...`
  regenerates it). Spot checks against the `.su` figures quoted in `ExpressionParser.cpp:30-49`: ParseInternal 80,
  ParseIdentifierExpression 280, ParseGeneralArray 360 match. Two figures quoted there are stale: GetObjectValueUsingTableNumber is 48
  in `ExpressionParser.cpp:48`, 56 in the ELF; `ObjectModel.cpp` quotes 56 for GetObjectValue_noTable, the ELF has 64. Both are within
  `StackUsage::Margin`
- Host functions are matched to board functions by their demangled name (normalised the same way in `board_stack.cpp` and
  `board-frames.py`). A function without a match counts 0: libc (newlib on the board), and functions that the board build inlined
- The budget is `FUZZ_STACK_BYTES` bytes (default 4096) below the stack pointer at the start of the evaluation, with a 128-byte task
  control block below the limit (`ObjectModel.cpp:640` takes the TCB end as the limit)
- Reports: `board-stack-overflow` (a function entry takes the virtual stack below the limit, so `CheckStack` did not prevent it),
  `no-stack-to-throw` (a C++ exception is thrown with less than `StackUsage::Throw` = 1050 bytes left, `GCodeException.h:16`; the
  target wraps `__cxa_throw`), `software-reset-stack-overflow` (`CheckStack` itself found too little stack to throw, the board resets).
  A stack overflow that `CheckStack` prevents ("Expression nesting too deep") is not a finding
- g++ calls the exit hooks while it unwinds, clang does not; the target resets the depth after every caught exception, so both builds
  count the same

## Input

Byte 0 modulo 11: the entry point, `Parse` (then `CheckForExtraCharacters` and `AppendAsString`, as `echo` does), `Parse` without
evaluation, `ParseFloat`, `ParseInteger`, `ParseUnsigned`, `ParseBoolean`, `ParseDriverId`, `ParseFloatArray`, `ParseIntArray`,
`ParseUnsignedArray`, `ParseDriverIdArray`. Byte 1: bits 0-4 + 1 = the array length for the array parsers (the array is exactly that
long), bit 7 = no G-code buffer. The rest is the expression text, in an allocation of exactly its length without a terminating null
(the parser gets its end pointer). The seeds are 1626 inputs made from about 300 expressions with several entry points.
`expression.dict` is a libFuzzer dictionary of the operators, functions, constants and object model keys.

The firmware builds do not use this folder: make and CMake only read `src/`, and the `*-fuzz` resource filter in `.project` hides it
from Eclipse.

## Build and run (WSL, g++ 13, clang 18 for libFuzzer)

    make              # build/expression-fuzz and build/asan-expression-fuzz (deterministic driver, ../sbc-packet-fuzz/fuzz_driver.inc)
    make check        # the seeds (both builds), then 20000 mutated inputs whose reports must all be in findings/known.txt, and make take-drop
    make take-drop    # build/asan-take-drop-check: fixed cases of take(), drop() and the array parsers (see below)
    make findings     # every input in findings/ must still report the key on its "# expect:" line
    make coverage     # gcov line coverage of the real RRF files after the seeds and RUNS (default 50000) mutated inputs
    make libfuzzer CLANGXX=clang++-18
    ASAN_OPTIONS=hard_rss_limit_mb=2048:detect_leaks=0 build/libfuzzer-expression-fuzz -dict=expression.dict -max_len=1024 corpus/
    FUZZ_STACK_BYTES=8192 build/asan-expression-fuzz ...    # another board stack budget
    build/asan-expression-fuzz -v <file>                    # print the result or the error message of one input

Every run should be bounded in memory (`ASAN_OPTIONS=hard_rss_limit_mb=2048`, the Makefile does that).

## Findings

The inputs are in `findings/`, each with its expected report and the expression in a comment. All defect lines are upstream code
(git blame in 6c45e07a7: David Crocker 2020-2026; every blamed commit is an ancestor of origin/3.7-dev and origin/3.7-ch) and the same
code is in origin/3.7-dev (line numbers there differ by a few lines). Every one is reached by the text of an expression alone, e.g.
`echo {65536 * 65536}` in a macro or job file; no corrupted packet and no wrong DSF is needed. This target starts at the parser, so the
way from a G-code line to it (StringParser, BinaryParser, and on the SBC whether DSF evaluates the expression itself or passes it to
RRF) was not checked here. Line numbers are those of 6c45e07a7.

| ID | Defect | Report | Effect on the board |
|----|--------|--------|---------------------|
| E1 | signed overflow of the int32 operators in `ParseInternal`: unary minus (`ExpressionParser.cpp:210`, "TODO overflow check" in the code), `+` 531, `-` 567, `*` 581 | UBSan signed overflow / negation at 210, 531, 567, 581 | undefined behaviour; the result on the board was not checked (expected: it wraps). Low |
| E2 | `mod(INT_MIN, -1)`: the check before `rslt.iVal %= nextOperand.iVal` (`ExpressionParser.cpp:2060`) only excludes 0 | UBSan division overflow at 2060 | undefined behaviour; on x86 it traps (SIGFPE), on the Cortex-M7 not checked. Low to medium |
| E3 | the float to int32 range checks of `floor`, `ceil`, `round` (`ExpressionParser.cpp:2034-2036`) and `pow` (2293-2295) are inclusive, `f <= (float)INT32_MAX` admits 2^31 | UBSan float-cast-overflow at 2036; `pow(2, 31)` gives INT_MIN, which then reaches RRFLibraries `SafeVsnprintf.cpp:368` (B6 of `../binary-code-fuzz`) | undefined behaviour, wrong value. Low |
| E4 | **Fixed, see "E4 fixed" below.** Nested object model arrays lost the parent index: `ParseArray` (`ExpressionParser.cpp:917-937`), `EvaluateTake` (1067-1083) and `EvaluateDrop` (1133-1157) build an `ObjectExplorationContext` without the indices of the enclosing arrays, unlike `ApplyObjectModelArrayIndex` (149), the `#` operator and `AppendAsString`. The array functions (`Move.cpp:104-105, 112`) then ask for `GetLastIndex()` / `GetIndex(1)` with too few indices | `internal-error` at `ObjectModel.cpp:603` (GetIndex) and 614 (GetLastIndex) | `REPORT_INTERNAL_ERROR`, and the function goes on with index 0: `take(move.axes[1].drivers, 1)` returns elements of axis 0. Wrong data, no memory error. Low. `(move.axes[1].drivers)[1]` is correct |
| E5 | **`vector(n, v)` with n >= 2^29, board only**: `ArrayHandle::Allocate` (`ArrayHandle.cpp:29-42`) computes `4 + n * 8` in the 32-bit `size_t`, which wraps (2^29 * 8 + 4 = 4); the space check passes, the uint16 `count` is truncated, and the loop constructs n elements from the start of a 2 KB heap block (`HeapBlockSize`, `Heap.cpp:20`). Reached from `vector` at `ExpressionParser.cpp:2239-2261` | none on the host (64-bit `size_t`: "Array too large") | a write of 2^29 * 8 bytes beyond a heap block, i.e. over the whole RAM: memory corruption, then a fault. Confirmed in the MB6HC ELF disassembly (0x4839dc-0x483a0a: `lsls r5,r4,#3; adds r5,#4`, `strh r4,[r0,#2]`, loop while `r3 < r4`); not run on a board. **High** |
| E6 | `SafeStrptime` takes `%m` without a range check (`strptime.cpp:144`, `tm_mon = ret - 1`) and indexes `_DAYS_BEFORE_MONTH[12]` with it (237, also 273). Reached by `datetime("2026-99-01T00:00:00")` (`ParseDateTime`, `ExpressionParser.cpp:2439`) | UBSan index out of bounds at 237; signed overflow at 144 for month INT_MIN | a read outside a static table of flash/RAM: wrong date. For a large month the address can lie outside memory; not checked. Low to medium |
| E7 | signed overflows in `SafeStrptime`: `%Y` of INT_MIN (`strptime.cpp:186`), `%d` of INT_MAX (285) | UBSan signed overflow | undefined behaviour, wrong date. Low |
| E8 | **Fixed, see "E8 fixed" below.** Throw with less than `StackUsage::Throw` left: `CheckStack(StackUsage::ParseGeneralArray)` (`ExpressionParser.cpp:132`) reserves 360 + 1050 + 300 bytes; `ParseGeneralArray` (360) calls `ParseInternal` (80) unchecked (994), which calls `ParseQuotedString` (312, `String<MaxStringExpressionLength>` on the stack) unchecked (196), which throws through `ThrowParseException` (32): 784 bytes, more than the 300 of `StackUsage::Margin`. Found by libFuzzer: `m[[1,[5,m[[1,[2,"` | `no-stack-to-throw` at 2639, 1024 of 1050 bytes left with the 4096 byte budget | the unwinder may write below the task stack, into the task control block below it (`ObjectModel.cpp:640`). How much stack the unwinder really needs, and so whether 26 to 124 missing bytes corrupt anything, was not checked. The input is tuned to the 4096 byte budget (4000 or 4200 are clean); on the board it has to be tuned to the stack in use when the expression starts, which was not done. Medium |

Observations without a sanitizer report:
- O1: `ObjectModel.cpp:80-84` ignores a failure of `gmtime_r`; a date-time beyond the year range is printed from uninitialised `tm`
  fields (`datetime(0) - 1` printed "2283416224-01-00T12:52:15" on the host). What newlib does on the board was not checked
- O2: stale stack figures, see "Stack model"
- O3: the `first_day` loop of `SafeStrptime` (`strptime.cpp:76-84`) runs once per year since 1970: 1e8 years took 2.9 s on the host,
  INT_MAX years about 21 times that. Whether the board watchdog fires was not checked
- O4: `BinaryParser.cpp:533-611` calls the array parsers without checking for `{`, `StringParser.cpp:1370` checks. Whether DSF can
  send such a parameter was not checked
- `labs(INT_MIN)` at `ExpressionParser.cpp:1937` is undefined only where `long` is 32 bits (the board); the 64-bit host cannot show it,
  and there is no 32-bit multilib in this WSL

## take() and drop() across a garbage collection (RV-2)

`take(s, n)` and `drop(s, n)` of a heap string held `p.Ptr()`, a raw pointer into the heap, and passed it to `StringHandle(s, len)`
(`EvaluateTake` and `EvaluateDrop`, `ExpressionParser.cpp:1111-1117` and `:1190-1196` in 6c45e07a7; there since 441bd498b, David
Crocker 2025-02-02, "Implemented take and drop for string values"; 65f8c54c9 only moved it).
`StringHandle::InternalAssign` first calls `Heap::AllocateSpace`, which garbage-collects when no heap block has room and enough space
waits to be recycled (`Heap.cpp:314-337`). The collection moves the strings down with `memmove` and adjusts only the handles
(`Heap.cpp:133-135`), so the copy then read from where the operand had been: wrong text, within the heap block. Found by the review
of RRF #97, #99 and #100; upstream `3.7-dev` and `3.7-ch` have the same code. Not seen on a board.

The fix copies from the handle: the new `StringHandle(const StringHandle& source, size_t offset, size_t len)` allocates first and then
reads the source through its handle, which the collection has adjusted. `take` and `drop` take the length with
`StringHandle::GetLength` and hold no pointer into the heap. The result is the same text as before whenever no collection runs.

`make take-drop` builds the target objects with a `main` of four fixed cases instead of the fuzz driver (`FUZZ_TAKE_DROP_CHECK`). Each
case lays out one heap block as garbage (1000 characters), the operand `var.op` (40 characters), the name of the variable and a filler
up to the end of the block, releases the garbage, and evaluates `take(var.op, 5)`, `take(var.op, 100)`, `drop(var.op, 30)` or
`drop(var.op, 1)` with the real parser. It checks the text of the result, and that exactly one garbage collection ran while the result
was allocated (`Heap::Diagnostics`), so a change of the heap that stops the case from collecting shows as a failure. Without the fix all
four cases fail: the result is filler text ("xxxxx" instead of "01234").

The fix removes four lines of `ExpressionParser.cpp` before the lines of E2, E3 and E8. The table above keeps the line numbers of
6c45e07a7; `findings/known.txt` and the `# expect:` lines have those of the current source.

## E8 fixed: stack checks before ParseQuotedString and in ParseGeneralArray

`CheckStack` is to be called "before making a recursive call, or before calling a function that needs a lot of stack from a recursive
function" (its comment in `ExpressionParser.cpp`), and `StackUsage::Margin` (300, `GCodeException.h:17`) is "the margin we allow for
calls to non-recursive functions that can throw". E8 breaks both:
- `ParseInternal` called `ParseQuotedString` without a check. Its frame is 312 bytes (28 pushed + 284, `String<MaxStringExpressionLength>`
  on the stack; MB6HC and Mini 5+ ELF), and with `ThrowParseException` (32) a throw from it needs 344 bytes, more than the margin.
- `ParseGeneralArray` called `ParseInternal` for the second and later elements without `CheckStack(StackUsage::ParseInternal)`. Every
  other call of `ParseInternal` has the check or a comment why an earlier check covers it.

The fix adds `StackUsage::ParseQuotedString` = 312 (both MCU branches, the same figure in both ELFs), `CheckStack` with it before
`ParseQuotedString`, and `CheckStack(StackUsage::ParseInternal)` before the element parse of `ParseGeneralArray`.

Behaviour change: with less than 1662 bytes of stack left (312 + `Throw` 1050 + `Margin` 300), any quoted string fails with
"Expression nesting too deep" before it is parsed, a valid, terminated one as well. The second and later elements of an array need
1430 bytes left (80 + 1350). Before, such a string or element was parsed when the stack was enough for it, and threw with too little
stack when it was not. This is the same margin that every other `CheckStack` keeps.

The frames of `ParseInternal` (80), `ParseGeneralArray` (360) and `ParseQuotedString` (312) in the MB6HC ELF are
unchanged. `findings/E8-unchecked-quoted-string-throw.hex` expects `clean` now, and the key is no longer in `findings/known.txt`, so
a recurrence shows as a new report.

Budget sweep of the E8 input (`FUZZ_STACK_BYTES` from 2000 to 6000 in steps of 4, 1001 runs): before the fix 27
budgets ended in `no-stack-to-throw`, with the fix all 1001 are clean.

## E4 fixed: the parent index of nested object model arrays

`ApplyObjectModelArrayIndex`, `Concat` (`^` of arrays), the `#` operator and `ExpressionValue::AppendAsString` (`ObjectModel.cpp`, "in
case it is a 2D array") put the parent index of an object model array (`param >> 8`) into the context before they read its elements.
`ParseArray` (behind `ParseFloatArray`, `ParseIntArray`, `ParseUnsignedArray` and `ParseDriverIdArray`), `EvaluateTake` and `EvaluateDrop` did
not, so array functions that read it (`context.GetIndex(1)`, or `GetLastIndex()` for the number of elements, as `Move.cpp:104-105` and `:112` do)
got the wrong index. The fix adds the same `AddIndex(param >> 8)` there. Behaviour change: `take`, `drop` and the array parsers give
the elements of the nested array that the expression names, as indexing does, and no longer report an internal error. The board frames
of `EvaluateTake` (128) and `EvaluateDrop` (200) in the MB6HC ELF are unchanged.

`make take-drop` has nine more cases: `take` and `drop` of `move.axes[1].drivers`, `move.axes[2].drivers` and
`move.axes[2].workplaceOffsets`, each compared with the same elements read with the index operator, and `ParseFloatArray` of
`{move.axes[2].workplaceOffsets}` and `ParseDriverIdArray` of `{move.axes[2].drivers}` and `{move.axes[0].drivers}` with the values of
the stand-in object model. Without the fix the run stops at the first of them with `internal-error GetLastIndex` (`ObjectModel.cpp:614`).
The four E4 inputs in `findings/` expect `clean` now, and the two internal-error keys are no longer in `findings/known.txt`.

## What was fuzzed (2026-11-24, RRF 3.7-dev 6c45e07a7)

- the seeds (1626 inputs, plain and ASan builds): no report
- deterministic driver, ASan build: seed 1 with 50000 inputs (no report), seed 3 with 50000 inputs (E1 at 531), seeds 4, 5 and 6 with
  200000 inputs each (E1 at 531, E4 GetIndex via `ParseUnsignedArray` without G-code buffer, E4 GetLastIndex via `ParseFloatArray`).
  The result of a run of seed 2 with 200000 inputs was not looked at
- libFuzzer, 3 workers, 330 s, about 714000 inputs, coverage 4772 edges: 56 crash artifacts, all with the keys of E1, E3, E6, E8 and
  B6 (581 x26, 567 x9, SafeVsnprintf 368 x8, 531 x5, strptime 237 x3, 210 x2, no-stack-to-throw x2, strptime 144 x1)
- line coverage after the seeds and seed 1 with 50000 inputs (`make coverage`): `ExpressionParser.cpp` 83 % of 1496 lines,
  `ObjectModel.cpp` 39 % of 936 (`ReportAsJson` and the other JSON code are not called by this target), `Variable.cpp` 31 % of 183
  (the target does not create or delete variables through G-code), `ArrayHandle.cpp` 45 % of 124, `StringHandle.cpp` 64 %,
  `Heap.cpp` 69 % (garbage collection is rarely reached), `strptime.cpp` 65 %

Not found: no AddressSanitizer report (no read or write beyond a buffer) in the real code on the 64-bit host, no deadlock, no
`board-stack-overflow` (`CheckStack` always caught deep nesting before a frame went below the limit), no file left open, no heap lock
left held.

Not reached or not modelled: the 32-bit-only defects (E5, `labs`) cannot run on the host; `fileread()` only with the 7 files of the
target; the G-code callers of the parser (`StringParser`, `BinaryParser`, `GCodes` meta commands `var`, `set`, `global`) are not part of
the target; object model values are those of the played tables, not those of a real machine; the deterministic byte mutator finds little
in text input, most findings came from the seeds and from libFuzzer.
