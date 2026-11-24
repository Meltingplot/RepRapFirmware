# strptime host test

This test runs RRF's `SafeStrptime` (`src/libc/strptime.cpp`), compiled unchanged with the RRFLibraries number code it uses, on a PC.

## Why

`SafeStrptime` stores the month of `%m` as `tm_mon = ret - 1` without a range check. Once year, month and day are set, it reads
`_DAYS_BEFORE_MONTH[timeptr->tm_mon]`, a table of 12 entries (`strptime.cpp:237`, and `:273` when the day is missing). So a month
outside 1 to 12 reads outside the table, and `-2147483648` also overflows `ret - 1`.

The text comes from the user:
- `datetime()` in an expression (`ExpressionParser.cpp`, format `%Y-%m-%dT%H:%M:%S`), e.g. `echo {datetime("2026-99-01T00:00:00")}`;
- `M905 P"<date>"` (`GCodes3.cpp`, format `%Y-%m-%d`);
- the time of an HTTP request in standalone mode (`HttpResponder.cpp`).

All callers treat a null result as an invalid date. `SafeStrptime` now refuses a month outside 1 to 12 that way.

Upstream code (the copy of newlib's strptime that David Crocker reduced, 03c7a6af4 is its last change). Found by the expression parser
fuzz target (finding E6, error class 3, 2026-11-24). What a read far outside the table does on the board depends on the address and was
not checked; on the host the month 2147483647 ends the run with SIGSEGV.

## What the test does

It parses five valid dates and checks year, month, day and day of the year, among them 31 December of a leap year and 1 March after a
leap day. It then gives six months that must be refused: 0, 13, 99, -1, 2147483647 and -2147483648.

Without the fix, the four small months are parsed and the run ends with SIGSEGV at 2147483647. The AddressSanitizer + UBSan build stops
at the first of them with "index -1 out of bounds" at `strptime.cpp:237`. With the fix, all 11 cases pass.

Not covered: the overflows of `%Y` and `%d` with values near INT_MIN or INT_MAX (finding E7, undefined behaviour and a wrong date, no
access outside an object).

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

To see the failure, run it against the source without the fix: `make clean check RRF=<checkout of the previous commit>`. The Makefile
reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
