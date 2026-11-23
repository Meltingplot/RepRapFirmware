# Census of the unmask allowlist

Each of the 21 entries of `unmask-allowlist.txt` was read in the code:
- for a call, every chain that its sites reach and the code of the region after the site;
- for a call through a register, every target that the firmware sets.

The verdict per entry is its comment in `unmask-allowlist.txt`; this file has the reasoning and the findings.

- **Basis:** 3.7-dev at b857e3530 (2026-11-22) with RRFLibraries 3.7-dev 888b479, configurations `Duet3_MB6HC` (20 entries),
  `Duet3_MB6XD` (16) and `Duet3Mini5plus` (13). Line numbers refer to that commit, except in a row that names a fix branch: there
  they refer to the tree of that branch.
- **Counts:** 3 SUSPECT, 4 SAFE-LAST, 5 SAFE-GUARDED, 3 SAFE-RESTORE, 6 SAFE-NO-UNMASK.
- **How:** `alloc-check.py --unmask` lists each site with the shortest chain, and `--unmask --query FUNCTION` lists every
  chain. For each site of an `indirect call` key, a script on the same ELF files listed every function with a call through a
  register that the site reaches, and every function on the way that passes it a target. An agent read those targets, with
  file and line for each, and the census checked them in the code.
- **Origin:** every entry is upstream code. Duet3D 3.7-dev a507cb8f0 has the same regions, except `DDARing::Hold` and the
  `CancelHomingMoveInFlight` call in `DDARing::StopMoves`. Those come with the feed hold of C. Hammacher's 3.7-ch branch
  (e7e30e13f, 2026-11-10), which the fork merged.

## Verdicts

| Verdict | Meaning |
|---|---|
| SUSPECT | The mask can be lifted while code that the region protects still runs. The fix branch is in brackets, or "open" |
| SAFE-LAST | The mask is lifted by the last call of the region, and nothing that the region protects runs after it. The entry has to be read again when its region changes, because a new call after it has the same key |
| SAFE-GUARDED | The path to the instruction runs only under a run-time condition that the check does not evaluate, and the condition excludes it inside the region |
| SAFE-RESTORE | The instruction that the check counts as a clear restores the state that its function saved |
| SAFE-NO-UNMASK | A call through a register: no target that the firmware sets lifts the mask |

## Facts that several entries share

- **Priorities** on these boards (3 priority bits, `src/RepRapFirmware.h:738-769`):
  - 0 watchdog, 1 Aux UART receive, 2 WiFi UART receive;
  - the FreeRTOS system call limit is 3 (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`, FreeRTOS
    `src/include/FreeRTOSConfig.h:323`);
  - 4 CAN, pins, ADC, driver diagnostics;
  - 5 step and DMA;
  - 6 USB and HSMCI, 7 SPI and Ethernet.

  `BasePriorityBooster(NvicPriorityStep)` keeps out 5 and below. Inside a `FromISR` function BASEPRI is the system call limit, so
  only 0 to 2 can run.
- **Ports.**
  - MB6HC and MB6XD build FreeRTOS with the Cortex-M7 r0p1 port (`Makefiles/Duet3_MB6HC.mk:85`, `Duet3_MB6XD.mk:85`). Its
    critical sections and `FromISR` functions run `cpsie i` (FreeRTOS `portmacro.h:210-224`, `:228-247`).
  - Mini5plus uses the Cortex-M4F port (`Makefiles/Duet3Mini5plus.mk:80`), which leaves PRIMASK alone.

  This is why the PRIMASK entries that end in `vPortEnterCritical` or a `FromISR` function are MB6HC/MB6XD only.
- **`xTaskResumeAll`** runs its body inside `taskENTER_CRITICAL()` and `taskEXIT_CRITICAL()` (`tasks.c:4038`, `:4162`), also when
  the scheduler stays suspended because an outer `TaskCriticalSectionLocker` holds it. So the end of every
  `TaskCriticalSectionLocker` writes BASEPRI 0, and on the r0p1 also clears PRIMASK.
- **`debugPrintf`** (`src/RepRapFirmware.cpp:211-224`):
  - With an M111 F debug buffer, it writes through `Platform::IsrDebugPutc` (`Platform.cpp:385`) with `RingBuffer::PutItem`,
    which only stores the item (RRFLibraries `RingBuffer.h:84-95`).
  - Otherwise it calls `Platform::DebugMessage`, which takes a mutex (`Platform.cpp:3362`), only
    `if (__get_BASEPRI() == 0 && !inInterrupt())` (`RepRapFirmware.cpp:219`).

  So with BASEPRI raised it never takes the mutex. It does not test PRIMASK.
- **`Move::Interrupt`** wakes the Move task with `WakeMoveTask` (`Move.cpp:1655-1671`; `Give` in task context) only right before
  it ends: `break` out of its loop when no DM is active or after a step error (`:2894-2895`), or `return` after the hiccup limit
  (`:2943-2944`). Nothing follows the loop (`:2974-2976`).

## Findings

CHX first: MB6HC in SBC mode.

| Entry | Boards | Verdict | Reason |
|---|---|---|---|
| `DDARing::LowPowerOrStallPause [primask] -> vPortEnterCritical` | MB6HC, MB6XD | **SUSPECT** [open] | `IrqDisable()` (`DDARing.cpp:1147`), then `GetCurrentDDA()` (`:1148`), whose `TaskCriticalSectionLocker` (`:566`) ends in `xTaskResumeAll`. Its critical section enables the interrupts again (r0p1), and the rest of the emergency pause up to `IrqEnable()` (`:1180`: `CancelStepping`, `CancelOutputs`, the queue walk) runs with the step interrupt enabled. Duet3D 3.7-dev a507cb8f0 `DDARing.cpp:795-796` has the same code (D. Crocker, 2024) |
| `TmcLoop [primask] -> vPortEnterCritical` | MB6HC | **SUSPECT** [open] | `AtomicCriticalSectionLocker lock2` (`TMC51xx.cpp:1988`); `TaskBase::ClearCurrentTaskNotifyCount` (`:1994`, `RTOSIface.h:236`) enters a critical section, which enables the interrupts (r0p1). `EnableEndOfTransferInterrupt`, `ResetSpi`, `EnableDma` and `EnableSpi` (`:1995-1998`) then run unmasked; the scheduler stays suspended (`:1979`). D. Crocker added lock2 (3fe403dde, 2023); Duet3D 3.7-dev a507cb8f0 `TMC51xx.cpp:1982-1992` is the same. What an interrupt there can break is not shown |
| `Move::UpdateCurrentMotion [primask] -> vPortEnterCritical` | MB6HC | **SUSPECT** [open] (census.md B2) | `DriveMovement::UpdateCurrentMotion` holds an `AtomicCriticalSectionLocker` (`DriveMovement.h:330`) and calls `NewSegment` (`:374`). `NewSegment` prints with `debugPrintf` under `DEBUG_DISCONTINUITIES` and M111 `PrintBadMoves` for an extruder (`DriveMovement.cpp:179-197`, `:206-222`). `PhaseStepControlLoop` calls it with BASEPRI 0 (`Move.cpp:2775`), so `debugPrintf` takes the mutex: the r0p1 enables the interrupts, and a wait would assert. Debug only |
| `Move::AddLinearSegments [basepri] -> vPortExitCritical` | all | SAFE-LAST | Sites:<br>- `Interrupt()` (`Move.cpp:2448`) is the last call of the second booster (`:2408-2453`);<br>- `MoveSegment::Split` (`:2209`) allocates only with an empty free list, and `MoveSegment::PrimeFreeList` (`:2178`) fills it (census.md, SAFE-PRIMED);<br>- `NewSegment` (`:2433`) prints with BASEPRI raised (guarded). |
| `Move::PhaseStepControlLoop [basepri] -> vPortExitCritical` | MB6HC | SAFE-LAST | The TMC task calls it (`TMC51xx.cpp:1915`). `Interrupt()` (`Move.cpp:2851`) is the last call of its booster (`:2848-2853`). The `UpdateCurrentMotion` site (`:2797`) prints with BASEPRI raised (guarded) |
| `Move::OnEndstopOrZProbeStatesChanged [basepri] -> vPortExitCritical` | all | SAFE-LAST | `CheckEndstops(true)` (`Move.cpp:4154`) is the last call of the booster (`:4128-4156`). It wakes the Move task only before it returns:<br>- at once for `stopAll` (`:3041-3049`);<br>- for `stopAxis`, only when no endstop is active any more (`:3060-3063`). The next `emgr.CheckEndstops()` then walks an empty list and returns no hit (`EndstopsManager.cpp:316-318`, `:368`), so `CheckEndstops` returns (`Move.cpp:3098-3103`).<br>The CAN sender is woken after the booster (`:4157`) |
| `RxDmaCompleteCallback [primask] -> vTaskGenericNotifyGiveFromISR` | MB6HC | SAFE-LAST | `tmcTask.GiveFromISR` is the last statement of the locker (`TMC51xx.cpp:1817-1822`). In the DMA interrupt (priority 5) the cleared PRIMASK lets in only priorities below 5, and the step interrupt has 5 too (`RepRapFirmware.h:759-760`) |
| `DDA::Prepare [basepri] -> vPortExitCritical` | all | SAFE-GUARDED | `CheckEndstops(false)` (`DDA.cpp:1492`) wakes the Move task only `if (executingMove)` (`Move.cpp:3041`, `:3060`) |
| `DDARing::PauseMoves [basepri] -> vPortExitCritical` | all | SAFE-GUARDED | Its only site is `FindStopPoint` (`DDARing.cpp:693`), which reaches `debugPrintf` with BASEPRI raised |
| `DDARing::Hold [basepri] -> vPortExitCritical` | all | SAFE-GUARDED (fixed in fix/hold-step-interrupt) | Inside `BasePriorityBooster(NvicPriorityStep)` (`DDARing.cpp:862`), `CancelHomingMoveInFlight` (`:864`) calls `GetCurrentDDAAlreadyLocked` (`:895`), which takes no lock. Before the fix it called `GetCurrentDDA`, whose `TaskCriticalSectionLocker` wrote BASEPRI 0 (see `xTaskResumeAll`), so the rest of the booster ran with the step interrupt enabled. The other sites reach a FreeRTOS call only through `debugPrintf`, which takes no mutex with BASEPRI raised:<br>- `CutHomingMove` (`:868`);<br>- `FindStopPoint` (`:872`);<br>- `GetEndCoordinates` (`:874`).<br>Their calls through a register go to the kinematics: `MotorStepsToCartesian`, `CartesianToMotorSteps`, `LimitSpeedAndAcceleration` and `GetTiltCorrection` (`Move.cpp:1289`, `:1303`, `DDA.cpp:598`, `:518`). A script checked every override in the three ELF files. None reaches a function that lifts a mask other than through `debugPrintf`, and none calls through a register. Feed hold, 3.7-ch e7e30e13f |
| `DDARing::StopMoves [basepri] -> vPortExitCritical` | all | SAFE-GUARDED (fixed in fix/hold-step-interrupt) | `CancelHomingMoveInFlight` (`DDARing.cpp:1099`), the first call of the booster (`:1098`), takes no lock any more. `FindStopPoint` (`:1101`) reaches only `debugPrintf`, as for `PauseMoves`. Feed hold, 3.7-ch e7e30e13f |
| `RepRap::StartIap [primask] -> FileStore::Close` | all | SAFE-GUARDED | `MassStorage::CombineName` (`RepRap.cpp:2280`) calls `MessageF` only when the name overflows. The `OpenFile` check before excludes that with the same `CombineName` (`RepRap.cpp:2059`, census.md) |
| `EraseAndReset [primask] -> efc_perform_command` | MB6HC, MB6XD | SAFE-RESTORE | `efc_perform_command` (CoreN2G `src/SAM4S_4E_E70/asf/sam/drivers/efc/efc.c:248-260`) uses `cpu_irq_save` and `cpu_irq_restore`. The restore runs `cpsie i` only if the saved state was enabled (`src/SAM4S_4E_E70/asf/common/utils/interrupt/interrupt_sam_nvic.h:151-167`). Erase before a reset |
| `Flash::WriteUserSignature [primask] -> efc_perform_command` | MB6HC, MB6XD | SAFE-RESTORE | As above (`Flash.cpp:545-550`) |
| `SoftwareReset [primask] -> efc_perform_command` | MB6HC, MB6XD | SAFE-RESTORE | As above, through `EraseAndReset` (`ExceptionHandlers.cpp:47`) and `NonVolatileMemory::EnsureWritten` (`:80`) |
| `CanDevice::Interrupt [primask] -> indirect call` | Mini5plus | SAFE-NO-UNMASK | `rxCallback` inside the locker (CoreN2G `CanDevice.cpp:1289-1290`) is the one that `CanDevice::Init` stores (`:265`). The firmware passes `StepTimer::CanTimingMessageReceived` (`CanInterface.cpp:344-349`), which stores three values and reads the timer registers (`StepTimer.cpp:214-221`, `:289-294`). The block is compiled only without an external timestamp counter (`CanDevice.cpp:1282`, CoreN2G `CoreIO.h:22`), so not on the SAME70 |
| `DDARing::LowPowerOrStallPause [primask] -> indirect call` | all | SAFE-NO-UNMASK | `CancelStepping` (`DDARing.cpp:1152`) reaches only `Move::IterateDrivers` with a register call. The lambdas passed to it on that path are those of `CancelStepping` (`Move.cpp:1137-1138`: set a flag, or nothing) and `StopAxisOrExtruder` (`:3378-3389`), which calls `CanMotion::StopDriverWhenExecuting` or `StopDriverWhenProvisional`. Those only edit the stop lists or the movement buffers (`CanMotion.cpp:351-365`, `:369-391`) |
| `DDARing::Hold [basepri] -> indirect call` | all | SAFE-NO-UNMASK (since fix/hold-step-interrupt) | `CancelHomingMoveInFlight` (`DDARing.cpp:864`) reaches `IterateDrivers` through `CancelStepping`, with the same lambdas as `LowPowerOrStallPause [primask] -> indirect call`. Before the fix the check did not report this call, because `GetCurrentDDA` had lifted the mask first |
| `DDARing::StopMoves [basepri] -> indirect call` | all | SAFE-NO-UNMASK (since fix/hold-step-interrupt) | As `DDARing::Hold [basepri] -> indirect call`, through `CancelHomingMoveInFlight` (`DDARing.cpp:1099`) |
| `Move::AddLinearSegments [basepri] -> indirect call` | all | SAFE-NO-UNMASK | Sites:<br>- `printf` (`Move.cpp:2194`), `AppendDetails` (`:2196`, `MoveSegment.cpp:61-73`) and, on MB6XD and Mini5plus, `LogStepError` under `ScheduleFirstSegment` (`:2433`, `DriveMovement.cpp:469-487`) reach `FormattedPrinter` through `StringRef` and `SafeVsnprintf` only. Its `putcharFunc` there is the lambda of `SafeVsnprintf`, which writes to the buffer (RRFLibraries `SafeVsnprintf.cpp:812-821`);<br>- `SetDirection` (`Move.cpp:2441`) reaches `IterateDrivers` with the lambda of `SetDriversDirection`, a pin write (`Move2.cpp:474`, `Move.h:905-916`). |
| `SoftwareReset [primask] -> indirect call` | all | SAFE-NO-UNMASK | `GetAsyncPort(n)->canWrite()` (`ExceptionHandlers.cpp:60`, `:62`). The ports are `AsyncSerial` objects (`Platform.cpp:369`, `:372`), and `AsyncSerial::canWrite` returns `RingBuffer::SpaceLeft` (CoreN2G `SAM4S_4E_E70/AsyncSerial.cpp:184-187`, `SAME5x_C21/AsyncSerial.cpp:101-104`; RRFLibraries `RingBuffer.h:109-112`) |
