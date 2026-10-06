# Plan: one lock for all G-code state (SBC mode concurrency)

Status: planned, nothing implemented yet
Branch: `feature/gcode-state-lock`, based on `3.7-dev` at `fc8b246`
All line numbers refer to that base commit.

## How to use this document

This is the work plan for the implementing session. It records what was analysed, what was decided and why,
and what still has to be verified. Work through section 6 (Phase 1) first, then section 7 (Phase 2). Each step lists
the files involved and how to tell that it is done. Items marked **verify** are assumptions that could not be
checked in the analysis session (no ARM toolchain, no RRFLibraries/CoreN2G checkout, no hardware).

## 1. Goal

Make it impossible by construction for code in one RTOS task to modify G-code state while another task is
modifying or reading it, without introducing deadlocks, and without every caller having to know which lock
protects which field. Violations must show up immediately in testing (assertion) instead of as rare races.

## 2. Background

In SBC mode, "Macro file X not found" has been seen since at least 3.4.x although the file exists. On the way to it
three races were fixed on `3.7-dev`:

- `d6b4384` Hold the channel mutex while requesting a macro file from the SBC
- `e1559cd` Do not start a trigger macro while the previous abort is unsent
- `74d1362` Use an SBC reply that arrives just as a macro or file request times out

The analysis that followed showed these are instances of a systematic problem: the rules for which task may touch
which G-code state exist only as a comment (`src/GCodes/GCodeBuffer/GCodeBuffer.h:390-411`) and are not enforced.
Many paths break them (section 4).

## 3. Current model and why it fails

### Tasks (src/Platform/TaskPriorities.h)

| Task | Priority | Touches G-code state |
|---|---|---|
| MAIN (`Tasks.cpp:283`, loop `Tasks.cpp:290-297`) | SpinPriority = 1 | yes, all channels, cooperatively |
| SBC (`SbcInterface.cpp:78`) | SbcPriority = 2 | yes, per packet channel |
| NETWORK (`Network.cpp:519`) | SpinPriority = 1 | urgent commands only (M108/M112), OM reads |
| Move (`Move.cpp:654`) | MovePriority = 4 | no; only the per-motion-system move handoff |
| HEAT, CAN, TMC, AIN, USB | higher | no (events go through `Event::AddEvent`) |

All channels (`File`, `File2`, `Trigger`, `Autopause`, `HTTP`, ...) are processed by the main task in turn
(`GCodes.cpp:476-488`). "Concurrent" execution of channels is cooperative: waits such as M109/M116 are state
machines that return every spin (`GCodes4.cpp:544-549`, `GCodes2.cpp:1996ff`), and M108 on another channel just sets
`cancelWait` (`GCodes2.cpp:1825` -> `GCodes.cpp:1282-1290`). Asynchronous motion systems run in parallel only in
the Move task and the step ISR; G-code for `File` and `File2` is interpreted by the main task. The move handoff is a
separate single-slot protocol (`ms.raw`, `__DMB()`, `segmentsLeft`: `GCodes.cpp:3503-3506`; consumer
`GCodes::ReadMove` `GCodes.cpp:3325ff` called from `Move.cpp:729/799`). None of this needs or may take the new lock.

### Per-channel mutex: origin and broken assumptions

From upstream history (Duet3D/RepRapFirmware):

- Up to 3.1 the SBC (then "Linux") interface ran inside the main task; no locks were needed.
- `37629b85` (2020-09-24, 3.2-dev, "Moved Linux subsystem to its own task") added `Mutex mutex` per GCodeBuffer,
  `macroSemaphore` with the blocking `RequestMacroFile`, `codesMutex`, and `resourceMutex` for `resourceOwners`;
  global actions were deferred to the main task with flags. The commit message notes "the heightmap access is NOT
  thread-safe yet". Per-channel granularity followed the protocol (every packet names one channel; DSF has one
  `Processor` with its own lock per channel), and it has one real benefit: while the main task blocks with a
  channel's mutex held (macro request wait, long CAN commands, see the comment the commit added at `ActOnCode`),
  the SBC task can still serve the other channels.
- `c2381b16` (2020-10-14) replaced `resourceMutex` with a `TaskCriticalSectionLocker`; `0986615f` (2022-11-01,
  3.5-dev, "Work on multiple motion systems") removed that as well. Current `LockResource`/`GrabResource`/
  `UnlockResource`/`UnlockAll` (`GCodes.cpp:5393-5519`) have no cross-task protection.
- 3.3 had `SbcPriority = 1; // TODO increase this when we are certain that it never spins.` `cef4926d`
  (2021-07-29, only in 3.4-dev, "Increased SBC task priority") raised it to 2 without changing the locking. With equal
  priority, a wake-up from the SPI interrupt does not preempt the main task immediately; since 3.4 every completed
  transfer preempts it at an arbitrary instruction. This plausibly explains why the symptoms date from 3.4.x
  (correlation, not proof).

The per-channel model assumes (a) the main task only touches the channel it is currently spinning and global state is
protected separately, and (b) the SBC task does not interrupt the main task mid-operation. Neither holds any more.
Much of the shared state belongs to no channel at all (`resourceOwners`, `pauseState`, `moveStates`, triggers,
`runningConfigFile`), so a per-channel mutex cannot protect it in principle.

## 4. Inventory of violations (verified in code)

Main task writing channels other than the one it spins, without that channel's mutex:

| Where | What |
|---|---|
| `GCodes.cpp:1061-1222` `DoAsynchronousPause` | pops/inits/unlocks the file channel(s) (`1126-1129`, `1149-1150`); callers hold only the autopause mutex (`GCodes4.cpp:1981`), the M25 channel's mutex (`GCodes2.cpp:1317`), or nothing (pause trigger `GCodes.cpp:951`) |
| `GCodes.cpp:398`, `962`, `3887` | `RunConfigFile`, `CheckTriggers`, `StartPrinting` call `DoFileMacro` outside `SpinGCodeBuffer`; `d6b4384` covers only the request fields, not `Push` etc. |
| `GCodes7.cpp:165-171` `MessageBoxClosed` | writes every channel via `GCodeBuffer::MessageAcknowledged` (`GCodeBuffer.cpp:1311-1331`); also reached from the message box timeout in `RepRap::Spin` (`RepRap.cpp:801`), outside `GCodes::Spin`. `messageAcknowledged` shares a bitfield byte with `abortFile`, `abortAllFiles`, `sendToSbc`, `messagePromptPending` (`GCodeBuffer.h:398-406`), which the SBC task clears under the channel mutex: lost-update race |
| `GCodes.cpp:424-430` `CheckFinishedRunningConfigFile` | copies state into all other channels |
| `GCodes.cpp:3522` `AbortPrint` | aborts the other file channel |
| `GCodes.cpp:5784ff` `SyncWith` | reads the other file channel (`syncState`, `IsLaterThan`, `OriginalMachineState()`) |
| `GCodes.cpp:1282-1290` `CancelWaitForTemperatures` | writes `cancelWait` of all channels; also called from the network task (`GCodeInput.cpp:331-340` -> `CheckForUrgentCommand` `:118`, `:167`) |
| `Platform.cpp:1031` `LowVoltagePause` | called from `Platform::Spin`, outside `GCodes::Spin` |

SBC task touching G-code state outside the documented handshake:

| Where | What |
|---|---|
| `SbcInterface.cpp:737-760` (Message) | `gb->SetFinished(true)` without lock |
| `SbcInterface.cpp:1683-1685` (`InvalidateResources`) | `gb->SetFinished(true)` without lock |
| `SbcInterface.cpp:309-316` (EmergencyStop/Reset) | `reprap.EmergencyStop()` -> `gCodes->EmergencyStop()` (`RepRap.cpp:1096`) -> `AbortPrint` on all channels (`GCodes.cpp:977-985`), no lock at all |
| `SbcInterface.cpp:580-635` (LockMovementAndWaitForStandstill, Unlock) | channel mutex only; `LockAllMovementSystemsAndWaitForStandstill` -> `LockResource` (`GCodes.cpp:1861`, `5393-5406`) writes the shared `resourceOwners` without protection -> two channels can both believe they own movement |

Consequences range from lost flag updates to use-after-free: an SBC handler that pops states (`InvalidateChannel`
`SbcInterface.cpp:825-858`, `InvalidateResources` `:1673-1700`, late `MacroCompleted(error)` `:543-548`) can run
while the main task is inside `GCodeBuffer::PopState` between `ms = machineState` (`GCodeBuffer.cpp:1095`) and
`machineState = ms->Pop()` (`:1108`).

Protocol ordering problems (not concurrency; Phase 2):

- The SBC task sends a blocking macro request (`SbcInterface.cpp:1455-1468`, lock-free) before pending aborts, and
  sends aborts only when the channel is not waiting and its mutex is free (`:1471-1509`). DSF then rejects a system
  macro (`fromCode == false`) because its stack still holds the aborted file
  (DSF `src/DuetControlServer/Link/Channel/Processor.cs` `DoMacroFile`, "System macro ... is requested but the stack
  is not empty") and replies `MacroCompleted(error)`; RRF reports "Macro file ... not found" if `reportMissing`.
  `e1559cd` fixed this for the trigger channel only (`GCodes.cpp:407-413`).
- The same race exists unfixed on the Autopause channel: `StartNextGCode` starts the next event when
  `IsDoingFile()` is false, ignoring a pending abort (`GCodes.cpp:605-614`). During `ProcessEvent` the main task holds
  the autopause mutex across `SysFileExists` and `RequestMacroFile`, so the abort cannot overtake the request.
  `ProcessEvent` uses `reportMissing = false` (`GCodes3.cpp:1432`), so the symptom is the default action (for a
  filament error: alert and pause without running `filament-error.g`), not "not found". Trigger: an event macro
  ending with M99 while another event is queued. (`abort` is executed by DSF itself and arrives as
  `InvalidateChannel`, DSF `Codes/Handlers/KeywordHandler.cs:168-171`, so it does not take this path.)

## 5. Design

### Rules

- **R1 Main task.** Holds the G-code state lock for every iteration of `reprap.Spin()` (`Tasks.cpp:295`) and during
  `reprap.Init()`. Every main-task path is covered, including the ones outside `GCodes::Spin` (message box timeout,
  `LowVoltagePause`). No caller needs to know about the lock.
- **R2 SBC task.** Takes the lock per `ExchangeData` (`SbcInterface.cpp:283`) once for the packet loop
  (`287-1287`) and once for the write phase (`1316-1570`), only with a timeout (`SbcYieldTimeout`, `:45`), and never
  across the delay in between (`1289-1310`) or across a transfer. Without the lock, every packet that is not on the
  lock-free list is resent (existing mechanism, `:1283-1286`). The per-channel mutex and its 12 `MutexLocker`
  sites in `SbcInterface.cpp` are removed.
- **R3 Lock-free list.** Only these may run without the lock:
  - storing code packets in the code buffer (`:320-410`, own critical section);
  - the per-channel handshake: macro request, macro result, `macroSemaphore`, close/abort notifications;
  - file operation results (`:1082-1226`) and their semaphores (`fileSemaphore`, `asyncWriteSemaphore`);
  - code replies (`gcodeReplyMutex`), IAP;
  - the hardware part of an emergency stop / reset.

  Only items on this list may give a semaphore the main task waits on.
- **R4 Urgent requests from other tasks.** Emergency stop from the SBC, network or CAN task switches off hardware at
  once and leaves the G-code part to the main task via a flag; the pattern exists already
  (`GCodes::emergencyStopCommanded`, `GCodes.cpp:59-67`, consumed at `:444-455`, currently only for
  `NUM_ASYNC_CHANNELS != 0`). M108 from the network task sets an atomic flag; the main task does the
  `CancelWaitForTemperatures` loop.

### Why this cannot deadlock

- There is a single lock for G-code state, so there is no lock order to get wrong.
- The main task blocks while holding it only on `macroSemaphore` (`GCodeBuffer.cpp:1264`), `fileSemaphore`
  (`SbcInterface.cpp:2258`) and `asyncWriteSemaphore` (`:2225`). Those are given only from lock-free-list packets.
- The SBC task acquires the lock only with a timeout and holds no other lock while trying. When it has the lock, the
  main task is parked at the top of its loop and holds nothing (malloc etc.).
- Lock order with the remaining mutexes is always "G-code state lock first": `gcodeReplyMutex` (main:
  `SbcInterface.cpp:2295/2331`; SBC write phase `:1319`, `:1504`), `fileMutex` (main only).

### Cost (the trade-off the per-channel design avoided)

While the main task blocks inside an iteration (macro request roundtrip, file operation, CAN reply), lock-requiring
packets of all channels are deferred instead of only those of the channel being spun: expression evaluation,
variables, lock/unlock, aborts, message prompts, sending codes to DSF, object model requests. Codes keep being
buffered and replies keep waking the main task. Measure this (Phase 1 step 7).

### Enforcement

- Runtime: an assertion "lock held by the calling task" in the mutating entry points of `GCodeBuffer` and in
  `GCodes::LockResource`/`GrabResource`/`UnlockResource`/`UnlockAll`; on failure `REPORT_INTERNAL_ERROR`. Keep it
  enabled in normal builds if cheap (one holder comparison).
- Compile time (optional): lock-free SBC code only gets the per-channel handshake object; access to a
  `GCodeBuffer` from `SbcInterface` only through a function that takes the lock guard as a parameter.

## 6. Phase 1 implementation steps

1. **Lock primitive.** New header, e.g. `src/GCodes/GCodeStateLock.h`: a `Mutex`, a blocking RAII locker, a
   try-locker with timeout, `IsHeldByCurrentTask()`, and `ASSERT_GCODE_STATE_LOCKED()`. Create the mutex before the
   SBC task starts. **verify** in RRFLibraries `RTOSIface.h`: `Mutex` is a FreeRTOS recursive mutex with priority
   inheritance, and how to get the holder (`GetHolder()` or `xSemaphoreGetMutexHolder`). The assertion must not fire
   before the scheduler runs (GCodeBuffers are constructed in `GCodes::Init`).
   Done when: compiles, unused.
2. **Main task (R1).** `Tasks.cpp:290-297`: hold the lock around `reprap.Init()` and around each `reprap.Spin()`.
   `RepRap::Spin` has no idle wait, so the lock is released once per iteration.
3. **SBC packet loop (R2/R3).** In `ExchangeData` try the lock before the packet loop; classify each request type as
   lock-free or locked; resend locked packets when the lock was not obtained. Split the mixed handlers: the
   handshake part of `MacroStarted` (`:784-821`), `MacroCompleted` (`:518-575`) and `InvalidateChannel`
   (`:825-858`) stays lock-free, the rest needs the lock (on resend, the handshake part finds nothing left to do).
   Release before the delay at `:1289`. Remove the `MutexLocker(gb->mutex, ...)` sites (`:495`, `:498`, `:541`,
   `:592`, `:622`, `:671`, `:842`, `:885`, `:1067`, `:1246`). Message handler `:737-760` (`SetFinished`) moves under
   the lock.
4. **SBC write phase.** Try the lock at the start of `:1316`. Without it, do only the lock-free part: code replies,
   `MacroFileClosed` (`:1448`), the blocking macro request (`:1455-1468`), invalidation bookkeeping. Remove the
   channel lock at `:1473`. `InvalidateResources` (`:1644-1700`): resolve waits first without the lock (as now),
   then take the lock blocking, then abort files.
5. **Remove the per-channel mutex.** `GCodeBuffer.h:329`, `GCodeBuffer.cpp:122`, `GCodes.cpp:533`,
   `GCodeBuffer.cpp:1251` (and the `lock.Release()` in `RequestMacroFile`). Update the access comment in
   `GCodeBuffer.h:390-411` to: lock-free handshake vs. everything else under the G-code state lock.
6. **Urgent requests (R4).** Generalise `emergencyStopCommanded` to all builds; `RepRap::EmergencyStop` does the
   hardware part and calls `gCodes->EmergencyStop()` only if the caller holds the lock, else sets the flag. Callers
   from other tasks: `SbcInterface.cpp:310`, `:315`, `Networking/MulticastDiscovery/MulticastResponder.cpp:70`,
   `CAN/CommandProcessor.cpp:354`, `:424`, urgent M112 via `GCodeInput.cpp`. M108 from `NetworkGCodeInput::Put`:
   set an atomic flag, main task runs `CancelWaitForTemperatures`. Watch `GCodes::DoEmergencyStop`
   (`GCodes.cpp:969-974`), which calls `reprap.EmergencyStop()` again.
7. **Assertions and diagnostics.** Add `ASSERT_GCODE_STATE_LOCKED()` to `GCodeBuffer::PushState`, `PopState`,
   `AbortFile`, `Init`, `SetFinished`, `PutBinary`/`Put`/`PutAndDecode`, `MessageAcknowledged`, the non-const
   `LatestMachineState()`, state setters, and to the resource functions in `GCodes.cpp:5393-5519`. Add counters to
   M122 (SBC section): packets deferred for the lock, longest wait for the lock.

Done when: builds for Duet 3 MB6HC/Mini5+ (SBC and standalone), the test matrix in section 9 passes, and no
`REPORT_INTERNAL_ERROR` from the assertion appears.

## 7. Phase 2: ordered notifications per channel

1. Replace `macroFileClosed`, `abortFile`/`abortAllFiles` and the request part of `requestedMacroFile`/
   `isWaitingForMacro` with a small per-channel FIFO in the handshake: `MacroFileClosed`, `Abort{all, emptyReply}`,
   `MacroRequest{name, fromCode, blocking}`. Main task enqueues (under the lock), SBC task drains lock-free in order
   in the write phase. A request can then never overtake an earlier close or abort on any channel.
2. Decide `emptyReply` at enqueue time. Today it is evaluated when the abort is sent (`SbcInterface.cpp:1498`:
   `!abortAll && GetState() == normal && (!lastCodeFromSbc || macroStartedByCode)`), so the outcome depends on
   timing. Check against DSF `Processor.FilesAborted` (M99 reply handling, `_suppressEmptyReply`).
3. Remove the special case in `IsTriggerBusy` (`GCodes.cpp:411`). This also fixes the Autopause race (section 4).

## 8. Out of scope / follow-ups

- Replies are not tied to requests (no request id). A stale or resent `MacroCompleted` can be taken as the answer to
  the next request (silent "empty macro" plus "Macro file has been started on channel ... but none was requested").
  Needs a protocol change with DSF.
- A late `MacroCompleted(error)` after a timeout pops whatever state is current (`SbcInterface.cpp:543-548`).
- `DoAsynchronousPause` pops file-channel macros without telling DSF until the pause completes
  (`GCodes4.cpp:2013-2020`, DSF `Processor.PrintPaused`). By design; handlers tolerate it.
- Standalone: object model reads from the network task (same priority as main) walk `GCodeBuffer` state
  (`GCodeBuffer.cpp:54-78`, e.g. `stackDepth`).

## 9. Verification

Build: needs the ARM toolchain and CoreN2G/RRFLibraries/FreeRTOS as described in `BuildInstructions.md`.

Test matrix on hardware, SBC mode, assertion enabled:

- print with M98 macros, tool changes, homing in the job
- M581 and M581.1 triggers whose macros end with M99, triggers firing back to back
- several events in a row (filament error on two extruders, driver warnings), event macros ending with M99
- M291 S2/S3 in a trigger macro, acknowledged with M292 from DWC; message box timeout
- pause via M25 from DWC, pause trigger 1, filament event; resume
- asynchronous motion systems (M596/M598) with sync points
- M109/M116 waiting, cancelled with M108 from another channel
- config.g with expansion boards (long CAN commands)
- emergency stop from DWC and PanelDue; DSF restart during a print

Standalone mode: the same where applicable, plus M108/M112 via HTTP/Telnet.

Compare M122 SBC counters (deferred packets, lock wait) before and after.

## 10. Open questions (verify)

1. RRFLibraries `Mutex`: recursive? priority inheritance? holder query API?
2. FreeRTOSConfig: `configUSE_TIME_SLICING` (relevant only for the history argument).
3. Any other task calling into `GCodes`/`GCodeBuffer` not listed in section 3? Re-check with grep for
   `GetGCodes()` and `GetGCodeBuffer(` in code that runs outside the main task.
4. Effect of deferring SBC object model requests during long main-task blocks on DWC refresh.

## 11. References

- DSF (Duet3D/DuetSoftwareFramework, `v3.7-dev`): `src/DuetControlServer/Link/Channel/Processor.cs` (`DoMacroFile`,
  `MacroFileClosed`, `FilesAborted`, `PrintPaused`, `Spin`), `src/DuetControlServer/Link/LinkService.cs`
  (sequential packet processing), `src/DuetControlServer/Codes/Handlers/KeywordHandler.cs` (`abort`).
- Upstream RRF commits: `37629b85`, `c2381b16`, `086956ba`, `0986615f`, `6200d42e`, `cef4926d`.
- Fork commits on `3.7-dev`: `d6b4384`, `e1559cd`, `74d1362`.
