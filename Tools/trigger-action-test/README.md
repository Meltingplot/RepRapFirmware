# Trigger action host test

This test checks what M581 and M581.1 triggers do when they fire, now that upstream has the K parameter. It covers the fork's
additions to M581.1 together with K: the action line `U`, the emergency stop by an action line that is just M112, the lock and the
fail-safe.

## Why

Upstream added `M581 K<n>` (`TriggerAction` in `src/GCodes/TriggerItem.h`; wiki `User_manual/Reference/Gcodes.md:7257`, `:7277`):
K0 emergency stop, K1 pause, K2 run `sys/trigger#.g`, K3 feed hold, K4 feed hold and abort. Without K, trigger 0 is K0, trigger 1 is K1
and all others are K2. `GCodes::CheckTriggers` executes K0, K4 and K3 in the pass that detects them, in that order and before it looks at
the trigger channel (`src/GCodes/GCodes.cpp:1033-1063`). K1 and K2 wait for the trigger channel (`:1065-1125`).

The fork had made a trigger with the action line `U"M112"` an emergency stop by setting the pending bit of trigger 0, and it treated
`number == 0` as "is the emergency stop trigger". The merge 7f67edf6b still did that (`GCodes.cpp:1024`, `GCodes3.cpp:428`, and
`TriggerItem.cpp:163`, `:189`, `:232`, `:365`, `:372` there). With K, trigger 0 can have another action, so:
- an M112 trigger did whatever trigger 0's K action was: a feed hold for K3, or trigger0.g for K2, and then only when the trigger channel
  was free;
- the lock did not cover K (`:189`). A K-only command, or one that failed after K, did not lock: the lock was a flag set inside
  `if (seen)` after a successful configuration (`:346-373`);
- trigger 0 with K2 could not have an action line, and a K3 trigger could;
- the fail-safe went by the trigger number, not by the action.

The fix:
- `TriggerItem::GetAction` returns `emergencyStop` for a trigger whose K action is `runMacro` and whose action line is M112
  (`TriggerItem.cpp:41-45`). `GetConfiguredAction` is the old body (`:47-54`);
- `CheckTriggers` and `CheckTrigger` (M582) set the trigger's own pending bit, as upstream (`GCodes.cpp:1024`, `GCodes3.cpp:428`);
- the fail-safe in `Check` (`TriggerItem.cpp:175`) and keeping the trigger after an error at M581.1 (`:382`) use
  `GetAction(number) == TriggerAction::emergencyStop`;
- the `locked` member is gone. `IsLocked` (`:56-60`) takes the lock from the state: a used trigger that is an emergency stop or has an
  action line. `Configure` checks it first and refuses K as well (`:199-211`), and the report shows it (`:451-454`). So the lock also
  holds after a command that failed, and for a trigger that is kept after an error at M581.1;
- a K that is not `runMacro` deletes the action line (`:236-240`), as M581 on inputs or endstops does (`:342-346`);
- `U` is refused unless the K action is `runMacro` (`:249-253`).

K is still read before the rest of the command (`:231-235`), as upstream does (5f54bb621). A command that fails later keeps its K.

## What the test compiles

The Makefile copies the real text out of the sources at build time (`extract.awk`, the same script as `Tools/sbc-wait-lock-test`,
with `#line` directives so that messages point at the real lines):
- `src/GCodes/TriggerItem.h` as it is, into `gen/TriggerItem.h`. Its includes are the stand-ins in `stubs/`;
- `src/GCodes/TriggerItem.cpp`: all functions but `EvaluateExpression`. `GetConfiguredAction` and `IsLocked` came with the fix and
  are extracted only if the header declares them;
- `src/GCodes/GCodes.cpp`: `CheckTriggers`;
- `src/GCodes/GCodes3.cpp`: `ConfigureTrigger` (M581, M581.1) and `CheckTrigger` (M582);
- `src/GCodes/GCodeBuffer/StringParser.cpp`: `CallsFileFunction`, which `IsAllowedAction` calls;
- `MaxTriggers` from `src/Config/Pins_Duet3_MB6HC.h` and `MaxGpInPorts` from CANlib `src/Duet3Common.h`. The bitmap types are those of
  the MB6HC, with RRFLibraries' real `Bitmap`.

`trigger_action_test.cpp` gives them stand-ins:
- `GCodeBuffer` parses a command line such as `M581.1 T5 P"door" U"M112"` and answers `Seen`, `GetIValue`, `GetUIValue`,
  `GetLimitedUIValue`, `TryGetLimitedUIValue`, `GetQuotedString`, `GetUnsignedArray` and `GetCommandFraction` from it. On the trigger
  channel it records `PutAndDecode`;
- `GCodes` records `DoEmergencyStop`, `StopAndAbortMotion`, `DoAsynchronousPause` and `DoFileMacro` with their arguments, and lets the
  test set `IsTriggerBusy`, `IsReallyPrinting` and `IsSimulating`. `DoEmergencyStop` initialises the triggers and clears the pending
  ones, as the real one does through `Reset` (`GCodes.cpp:1137`, `:287-291`);
- `Move` records `FeedHold` and lets the test set `IsFeedHoldActive`. The test sets the input ports;
- `EvaluateExpression` returns a value that the test sets per expression, or throws `GCodeException` as an unknown variable does.

## The checks

1. Trigger 5 is `M581.1 T5 P"door" U"M112"`, trigger 0 is `M581 T0 P1 S1 K3` or `K2`. When the expression becomes true, the first
   `CheckTriggers` calls `DoEmergencyStop` and nothing else, also while the trigger channel is busy. `M582 T5 S1` does the same.
2. `M581 T0 K2` is refused with "locked" when trigger 0 is configured on an input (K0 by default), and trigger 0 stays an emergency stop.
   `M581.1 T5 K3` and `M581 T5 K3` are refused when trigger 5 has an action line.
3. `M581 T5 P2 S1` does not lock trigger 5. A following `M581 T5 K0` does: `M581 T5 P-1` is then refused and the trigger stays.
   The same holds after a command that sets K0 and then fails: `M581.1 T5 K0 U"M42 P0 S1"` is refused ("does not run a macro
   file"), and `M581.1 T5 K0 D70000` throws.
4. `M581.1 T5 K3 P"x" U"M42 P0 S1"` is refused ("does not run a macro file") and leaves trigger 5 without an expression and an action
   line. `M581.1 T0 K2 P"x" U"M42 P0 S1"` on an unused trigger 0 is accepted. When it fires, the line waits for the busy trigger channel,
   then goes to the trigger channel by `PutAndDecode`; there is no emergency stop and no `trigger0.g`.
   `M581.1 T5 U"M112"` on an unused trigger 5, then `M581.1 T5 K1 P"door"`, `K3` or `K4`: both are accepted and the action line is
   gone, also from the report. When the expression becomes true while printing, `CheckTriggers` calls `DoAsynchronousPause`,
   `FeedHold` or `StopAndAbortMotion`, and not `DoEmergencyStop`.
5. The fail-safe:
   - `M581.1 T5 K0 P"probe"`, then the expression fails: `Check` returns true, the report still says "fires when", and the message is
     "Trigger 5 fired";
   - `M581.1 T0 K2 P"probe"`, then the expression fails: `Check` returns false, the report says "if enabled would fire" (condition -1),
     and the message is "Trigger 0 disabled";
   - the expression fails already at M581.1: the command throws, `T5 K0` is kept and its next `Check` fires, `T0 K2` is cleared;
   - the kept trigger is locked: after `M581.1 T5 K0 P"probe"` and after `M581.1 T0 P"probe"` with a failing expression the report
     says "locked until restart", and `M581 T5 P-1` or `M581 T0 P-1` is refused.
6. What upstream does (`GCodes.cpp:1017-1131`):
   - K3: `Move::FeedHold` once, `feedHoldTriggerActive` true while the input is active and false after it; no feed hold while simulating;
   - K4: `StopAndAbortMotion` with the autopause channel as the requester;
   - trigger 1 without K: `DoAsynchronousPause(trigger channel, PrintPausedReason::trigger, GCodeState::pausing1)` and the alert while
     printing. When not printing, or while a feed hold is active, it is dropped: it does not pause later either;
   - K2 trigger 3: `DoFileMacro(trigger channel, "trigger3.g", true, AsyncSystemMacroCode)`; trigger 0 without K: `DoEmergencyStop`;
   - K4 trigger 3 before K3 trigger 2 in the same pass. With K0 trigger 9 pending as well, only `DoEmergencyStop` runs;
   - the report of `M581 T2` on an unused trigger 2 starts with "Trigger 2 (K2, run macro) " (`TriggerItem.cpp:390-398`).

The wiki says that an expression error after M581.1 disables the trigger (`Gcodes.md:7353`). The fork's fail-safe in 5 deviates from that
for emergency stop triggers on purpose, see DSF `design/rrf-local-trigger-actions.md`.

## Running

    make check

builds the test plain and with AddressSanitizer and UBSan, and runs both (g++ 13, WSL). UBSan stops at the first report.
`TRIGGERITEM_H`, `TRIGGERITEM_CPP`, `GCODES_CPP` and `GCODES3_CPP` select the files under test. For the text from before the fix:

    git show 7f67edf6b:src/GCodes/TriggerItem.h > /tmp/old/TriggerItem.h     # and TriggerItem.cpp, GCodes.cpp, GCodes3.cpp
    make check TRIGGERITEM_H=/tmp/old/TriggerItem.h TRIGGERITEM_CPP=/tmp/old/TriggerItem.cpp \
               GCODES_CPP=/tmp/old/GCodes.cpp GCODES3_CPP=/tmp/old/GCodes3.cpp

`RRFLIBS` and `CANLIB` point at RRFLibraries and CANlib, by default next to this repository. The firmware builds do not use this folder:
make and CMake only read `src/`, and the `*-test` resource filter in `.project` hides it from Eclipse.

Without the fix (7f67edf6b) 22 of the 33 checks fail: all of 1 to 5. The 11 checks of 6 pass on both. The ASan build reports nothing on
either. Before the fix, the sequence `M581.1 T5 U"M112"`, `M581.1 T5 K1 P"door"` in 4 made an emergency stop through the pending bit
of trigger 0 (with trigger 0 at its default). With the fix it pauses, because the K1 deletes the action line.

## Not covered

- The expression parser: `EvaluateExpression` is replaced, so the heap lock and `ExpressionParser` are not compiled.
- What the called functions do: `DoEmergencyStop`, `StopAndAbortMotion`, `FeedHold`, `DoAsynchronousPause` and `DoFileMacro` are
  stand-ins. The test checks that `CheckTriggers` calls them, and with which arguments.
- How the trigger channel runs an action line: `CheckMetaCommand` returns false, and the check of further commands on the line
  (`GCodes2.cpp:136`) is not compiled.
- M24 while the input of a feed hold trigger is active (`GCodes2.cpp:1227-1235`) and M25 while a feed hold is active (`:1366-1374`).
- Endstop triggers. The stand-in has the axes X, Y and Z and the tests use inputs only. A machine with an axis letter that M581.1 also
  uses as a parameter (for example U) is not covered.
- The confirmation time `D`: the clock does not move.
- SBC mode: DSF takes over an M581.1 whose expression reads `sbc.*` fields (DSF `SbcTriggerService.cs`).
