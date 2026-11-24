# INT_MIN print host test (RRFLibraries SafeVsnprintf)

This test checks that `SafeSnprintf` of RRFLibraries prints the most negative `int` with `%d` / `%i` and the most negative `long long`
with `%lld` without undefined behaviour. It also checks that the text is right.

## Why

`FormattedPrinter::PrintI` and `FormattedPrinter::PrintLL` (`src/General/SafeVsnprintf.cpp:368` and `:325` in RRFLibraries 3.7-dev
888b479) negated a negative value with `u = -i`. For INT_MIN and LLONG_MIN that negation overflows, which is undefined behaviour. The
fix negates the unsigned copy instead (`u = -u`), which C++ defines (modulo 2^N) and which gives the magnitude for every negative value.
The fix is in the RRFLibraries repository; this test is here because RRFLibraries has no tools folder and no Eclipse filter for one.

RRF reaches it from a correct DSF. `BinaryParser` prints the G-code number and the integer parameters of a binary code with `%d`
(`PrintCommand`, `AppendFullCommand`, `WriteParameters`), so a code number or parameter of -2147483648 does it. This is finding B6 of
`Tools/binary-code-fuzz`.

Upstream code (RRFLibraries 9fdd27ec, 2018). The effect on a board, wrong text or something else, has not been seen or checked. On the
host the program without the fix prints the right text at -O1; only UBSan shows the overflow.

## What the test compiles

`src/General/SafeVsnprintf.cpp` and `src/General/Strnlen.cpp` of RRFLibraries, unchanged, with `print_int_min_test.cpp`. The test
prints 14 values with `%d`, `%i`, `%u`, `%x`, `%lld` and `%llu`, among them INT_MIN, INT_MIN + 1, INT_MAX, LLONG_MIN, LLONG_MIN + 1 and
LLONG_MAX, and compares each text with the expected one. `make check` runs it plain and with UBSan, which stops at the first undefined
behaviour (`-fno-sanitize-recover=all`).

Without the fix the UBSan run stops at `SafeVsnprintf.cpp:368` (`%d INT_MIN`, the first case). Built with recover on, it reports both
places: 368 (`int`) and 325 (`long long`). With the fix, both runs pass all 14 cases.

Not covered:
- `%ld`: RRFLibraries reads it with `va_arg(args, int)` (the target's `long` is 32 bits), which a 64-bit host cannot pass cleanly;
- the float, string and padding conversions;
- the target compiler and its code for the old negation.

The Makefile reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The firmware builds do
not use this folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build the plain and the UBSan program, then run both

To see the failure, run it against RRFLibraries without the fix: `make clean check RRFLIBS=<checkout of 3.7-dev 888b479>`. The objects
depend on the source files, not on `RRFLIBS=`, so `make clean` comes first.

Each case prints `PASS` or `FAIL` with the text. The exit code is 1 if a case fails or UBSan stops the run.
