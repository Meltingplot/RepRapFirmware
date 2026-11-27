# Feed hold M24 host test

This test runs RRF's real handling of M24 during a feed hold on a PC. The Makefile copies three parts out of the source at build time,
as `Tools/queued-code-buffer-test` does:
- `RegularGCodeInput::CheckForUrgentCommand` from `src/GCodes/GCodeInput.cpp`;
- `enum class GCodeInputState` from `src/GCodes/GCodeInput.h`;
- the feed hold block of `case 24:` in `GCodes::HandleMcode` from `src/GCodes/GCodes2.cpp`.

`feed_hold_m24_test.cpp` gives them stand-ins for the parts of `Move`, `GCodes` and `RepRap` they use, with counters for what they call.

## What is tested and why

M24 releases an active feed hold (M25.1). Upstream 2092f4148 ("M24 during a feed hold no longer resumes a paused job") changes what
else it does when a print is paused as well. Before that commit, M24 resumed the paused print too. Since then, M24 only releases the
hold, and a paused print stays paused until an M24 of its own. Two places changed:

- **The urgent command scanner** of the serial, USB and network inputs (`GCodeInput.cpp:158-168`). It is called for each received
  character (`GCodeInput.cpp:390` and `663`).
  - What it does: a plain M24 (no parameter, ended by a line end or `;`) during a hold, while no feed hold trigger is active, releases
    the hold. The scanner then resets its input, so the line is swallowed. A queued copy might never run, because the channel may be
    the one whose macro the hold has stopped.
  - Before 2092f4148, the scanner did this only while no print was paused. During a paused print it left M24 to `HandleMcode`. In
    standalone mode that M24 was queued behind the held macro.
- **The hold block of M24 in `HandleMcode`** (`GCodes2.cpp:1228-1243`). Every M24 the scanner leaves alone gets here, including all
  M24 from the SBC.
  - What it does: if a feed hold trigger is still active (set in `GCodes::CheckTriggers`, `src/GCodes/GCodes.cpp:1031`), M24 is
    refused. Otherwise it releases the hold and gives the held streams their read-ahead positions back.
  - Before 2092f4148, it then went on, while a print was paused, to the pause and resume code below it, which resumes the print
    (`GCodes2.cpp:1260-1278`, `resume.g`). Now it always stops after the release.

The test feeds lines to the scanner character by character and runs the hold block inside a `switch`, as `HandleMcode` has it. A
`break` ends M24, and falling off the end of the block means M24 carries on to the resume code. The cases are:

| Case | Part | 2092f4148 |
|---|---|---|
| plain M24, hold, print paused: released and swallowed | scanner | changed |
| M24 with a comment, hold, print paused: released and swallowed | scanner | changed |
| plain M24, hold, no print paused: released and swallowed | scanner | as before |
| plain M24, hold, trigger active: left to `HandleMcode` | scanner | as before |
| plain M24, no hold: left to `HandleMcode` | scanner | as before |
| M24 with a parameter, hold: left to `HandleMcode` | scanner | as before |
| hold, print paused: released, read-ahead restored, stops, print stays paused | `HandleMcode` | changed |
| hold, no print paused: released and stops | `HandleMcode` | as before |
| hold, trigger active: refused, nothing released | `HandleMcode` | as before |
| no hold, print paused: goes on to the resume code | `HandleMcode` | as before |

Built against the text of fork 3.7-dev 68fa67b8a, before the fix, exactly the three cases marked "changed" fail:
- the two scanner cases fail with "the scanner did not swallow the line, so it is queued behind the held code" and "the scanner
  released the hold 0 times instead of 1";
- the `HandleMcode` case fails with "M24 carried on to the pause and resume code, which resumes the paused print".

DSF has the matching change in upstream DSF ae85dd5b5 ("M24 that only releases a feed hold keeps a paused job paused"). The SBC side is
tested there.

## Running

```
make check
```

To run it against other files, for example copies from before the fix:

```
make clean
make GCODEINPUT_H=<file> GCODEINPUT_CPP=<file> GCODES2_CPP=<file> check
```
