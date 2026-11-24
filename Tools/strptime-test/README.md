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

The year of `%Y` had no limit either. When year, month and day are set, `SafeStrptime` calls `first_day` (`strptime.cpp:76-84`),
which loops once per year since 1970, so `datetime("2147483647-01-01T00:00:00")` runs about 2.1e9 iterations in the main task. On the
host that took about 20 s. On the board, the MB6HC build of this loop has 15 instructions per year and no division (300 MHz,
`conf_clock.h:89` of CoreN2G); from that I estimate one to two minutes, not measured. `RepRap::Tick` resets the board with
`stuckInSpin` when the main task has not run for 20 s (`RepRap.h:211`, `RepRap.cpp:1299`). Every caller passes the result to newlib's
`mktime`, which returns -1 for a `tm_year` beyond +/- 10000 (in the MB6HC build, `tm_year + 10000 > 20000` right after
`validate_structure`), so such a year never gave a valid date. `SafeStrptime` now refuses a year outside 1900 +/- 10000 as well.
This also removes the overflow of `ret - 1900` for a year near INT_MIN.

Upstream code (the copy of newlib's strptime that David Crocker reduced, 03c7a6af4 is its last change). Found by the expression parser
fuzz target (findings E6 and E7, observation O3, error class 3, 2026-11-24). What a read far outside the table does on the board
depends on the address and was not checked; on the host the month 2147483647 ends the run with SIGSEGV.

## What the test does

It parses seven valid dates and checks year, month, day, day of the year and day of the week, among them 31 December of a leap year,
1 March after a leap day and the first and last year that `mktime` accepts (-8100 and 11900). It then gives six months that must be
refused: 0, 13, 99, -1, 2147483647 and -2147483648, and five years: 11901, -8101, 100000000, 2147483647 and -2147483648.

Without the month fix, the four small months are parsed and the run ends with SIGSEGV at 2147483647. The AddressSanitizer + UBSan build
stops at the first of them with "index -1 out of bounds" at `strptime.cpp:237`. Without the year fix, the five years are parsed and the
plain build needs about 20 s. With both fixes, all 18 cases pass at once.

Not covered: the overflow of `%d` with a day near INT_MAX (finding E7, undefined behaviour and a wrong date, no access outside an
object).

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both

To see the failure, run it against the source without the fix: `make clean check RRF=<checkout of the previous commit>`. The Makefile
reads RRFLibraries from next to this repository, `../../../RRFLibraries` (override with `RRFLIBS=`). The firmware builds do not use this
folder: make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.
