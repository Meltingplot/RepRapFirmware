# Census of the unmask allowlist

Each of the 20 entries of `unmask-allowlist.txt` was read in the code:
- for a call, every chain that its sites reach and the code of the region after the site;
- for a call through a register, every target that the firmware sets.

The verdict per entry is its comment in `unmask-allowlist.txt`; this file has the reasoning and the findings.

- **Basis:** 3.7-dev at b857e3530 (2026-11-22) with RRFLibraries 3.7-dev 888b479, configurations `Duet3_MB6HC` (19 entries),
  `Duet3_MB6XD` (15) and `Duet3Mini5plus` (13). Line numbers refer to that commit, except in a row that names a fix branch: there
  they refer to the tree of that branch.
- **Fixed:** `DDARing::LowPowerOrStallPause [primask] -> vPortEnterCritical` (SUSPECT, MB6HC and MB6XD) went away with
  fix/emergency-pause-interrupts: between `IrqDisable` and `IrqEnable` it calls `GetCurrentDDAAlreadyLocked`, which takes no lock.
- **Fixed with Meltingplot/FreeRTOS#1 (d3ce7a3):** `TmcLoop [primask] -> vPortEnterCritical` (SUSPECT, MB6HC), see "With the
  restoring ports" below. With d3ce7a3 pinned, `make update` removed it and the 9 other keys that end in `vPortExitCritical`,
  `vPortEnterCritical` or `vTaskGenericNotifyGiveFromISR` (2026-11-23, 3.7-dev 2e31913b0). `unmask-allowlist.txt` now has 22
  entries: the 10 other entries of the first table and the 12 new ones of that section.
- **Counts:** 1 SUSPECT, 4 SAFE-LAST, 6 SAFE-GUARDED, 3 SAFE-RESTORE, 6 SAFE-NO-UNMASK.
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
  - 0 watchdog;
  - on the Mini5plus 1 Aux UART receive, 2 WiFi UART receive, 3 their transmit; on MB6HC and MB6XD 3 Aux and WiFi UART;
  - the FreeRTOS system call limit is 3 (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`, FreeRTOS
    `src/include/FreeRTOSConfig.h:323`);
  - 4 CAN, pins, I2C, TMC UART, and on the Mini5plus ADC and driver diagnostics;
  - 5 step and DMA;
  - 6 USB and HSMCI, 7 SPI and Ethernet.

  `BasePriorityBooster(NvicPriorityStep)` keeps out priorities 5 to 7. Inside a `FromISR` function BASEPRI is the system call
  limit, so only 0 to 2 can run.
- **Ports.**
  - MB6HC and MB6XD build FreeRTOS with the Cortex-M7 r0p1 port (`Makefiles/Duet3_MB6HC.mk:85`, `Duet3_MB6XD.mk:85`). Its
    critical sections and the `FromISR` functions that mask run `cpsie i` (FreeRTOS `portmacro.h:210-224`, `:228-247`).
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

  So with BASEPRI raised it never takes the mutex. Since fix/debugprintf-primask it also tests PRIMASK
  (`__get_PRIMASK() == 0`, as `GetMallocMutex` does in `Tasks.cpp:117`), so with PRIMASK set it never takes the mutex either.
- **`Move::Interrupt`** wakes the Move task with `WakeMoveTask` (`Move.cpp:1655-1671`; `Give` in task context) only right before
  it ends: `break` out of its loop when no DM is active or after a step error (`:2894-2895`), or `return` after the hiccup limit
  (`:2943-2944`). Nothing follows the loop (`:2974-2976`).

## Findings

CHX first: MB6HC in SBC mode.

| Entry | Boards | Verdict | Reason |
|---|---|---|---|
| `TmcLoop [primask] -> vPortEnterCritical` | MB6HC | **SUSPECT** [fixed by Meltingplot/FreeRTOS#1] | `AtomicCriticalSectionLocker lock2` (`TMC51xx.cpp:1988`); `TaskBase::ClearCurrentTaskNotifyCount` (`:1994`, `RTOSIface.h:236`) enters a critical section, which enables the interrupts (r0p1). `EnableEndOfTransferInterrupt`, `ResetSpi`, `EnableDma` and `EnableSpi` (`:1995-1998`) then run unmasked; the scheduler stays suspended (`:1979`). D. Crocker added lock2 (3fe403dde, 2023); Duet3D 3.7-dev a507cb8f0 `TMC51xx.cpp:1982-1992` is the same. What an interrupt there can break is not shown |
| `Move::UpdateCurrentMotion [primask] -> vPortEnterCritical` | MB6HC | SAFE-GUARDED (fixed in fix/debugprintf-primask; census.md B2) | `DriveMovement::UpdateCurrentMotion` holds an `AtomicCriticalSectionLocker` (`DriveMovement.h:330`) and calls `NewSegment` (`:374`). `NewSegment` prints with `debugPrintf` under `DEBUG_DISCONTINUITIES` and M111 `PrintBadMoves` for an extruder (`DriveMovement.cpp:179-197`, `:206-222`). `PhaseStepControlLoop` calls it with BASEPRI 0 (`Move.cpp:2775`). Before the fix `debugPrintf` took the USB mutex there (`Platform.cpp:3362`): the r0p1 enabled the interrupts inside the locker, and if another task held the mutex the TMC task waited there. Now it tests PRIMASK (`RepRapFirmware.cpp:219`): with an M111 F buffer it stores the text, otherwise it drops it. The check does not evaluate that test, so the key stays |
| `Move::AddLinearSegments [basepri] -> vPortExitCritical` | all | SAFE-LAST | Sites:<br>- `Interrupt()` (`Move.cpp:2448`) is the last call of the second booster (`:2408-2453`);<br>- `MoveSegment::Split` (`:2209`) allocates only with an empty free list, and `MoveSegment::PrimeFreeList` (`:2178`) fills it (census.md, SAFE-PRIMED);<br>- `NewSegment` (`:2433`) prints with BASEPRI raised (guarded). |
| `Move::PhaseStepControlLoop [basepri] -> vPortExitCritical` | MB6HC | SAFE-LAST | The TMC task calls it (`TMC51xx.cpp:1915`). `Interrupt()` (`Move.cpp:2851`) is the last call of its booster (`:2848-2853`). The `UpdateCurrentMotion` site (`:2797`) prints with BASEPRI raised (guarded) |
| `Move::OnEndstopOrZProbeStatesChanged [basepri] -> vPortExitCritical` | all | SAFE-LAST | `CheckEndstops(true)` (`Move.cpp:4154`) is the last call of the booster (`:4128-4156`). It wakes the Move task only before it returns:<br>- at once for `stopAll` (`:3041-3049`);<br>- for `stopAxis`, only when no endstop is active any more (`:3060-3063`). The next `emgr.CheckEndstops()` then walks an empty list and returns no hit (`EndstopsManager.cpp:316-318`, `:368`), so `CheckEndstops` returns (`Move.cpp:3098-3103`).<br>The CAN sender is woken after the booster (`:4157`) |
| `RxDmaCompleteCallback [primask] -> vTaskGenericNotifyGiveFromISR` | MB6HC | SAFE-LAST | `tmcTask.GiveFromISR` is the last statement of the locker (`TMC51xx.cpp:1817-1822`). In the DMA interrupt (priority 5) the cleared PRIMASK lets in only priorities 0 to 4, and the step interrupt has 5 too (`RepRapFirmware.h:759-760`) |
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

## With the restoring ports (Meltingplot/FreeRTOS PR #1)

From Meltingplot/FreeRTOS PR #1 on, the critical sections restore the mask that they found, and the trace hooks where a task
blocks assert `xPortTaskCanBlock()` (see README.md). The check then finds no lift in a critical section any more, and reports the
calls that can block inside a region instead (target `xPortTaskCanBlock`).

- **Basis:** 3.7-dev at d68586f10 (2026-11-23) with RRFLibraries 3.7-dev 888b479, built once against FreeRTOS 3.7-dev 980af10
  and once with PR #1 (783d4c5), configurations `Duet3_MB6HC`, `Duet3_MB6XD` and `Duet3Mini5plus`. Line numbers refer to
  d68586f10.
- **Gone with PR #1:** the 10 keys that end in `vPortExitCritical`, `vPortEnterCritical` or `vTaskGenericNotifyGiveFromISR`,
  among them `TmcLoop [primask] -> vPortEnterCritical` (SUSPECT): `ClearCurrentTaskNotifyCount` no longer enables the interrupts
  inside `lock2`.
- **New with PR #1:** 12 keys. 5 calls through a register were there before, but a site reports a call through a register only
  if it does not reach a lift (alloc-check.py, `unmask_findings`), and the lift of the critical section hid them. 7 keys are
  calls that can block.
- **How:** for each site of a `-> xPortTaskCanBlock` key, a script took the chains of `alloc-check.py`'s model again, but did
  not let them pass through `debugPrintf`, `Tasks::AllocPermanent` and `Platform::MessageF`. No site reached `xPortTaskCanBlock`
  then. So every block on these paths is behind one of those three functions, and the reasons below say why none runs inside the
  region. The shortest chains end in a mutex give (`MutexLocker::Release`, `Mutex::Release`), which does not block
  (`queueMUTEX_GIVE_BLOCK_TIME` is 0, FreeRTOS `src/queue.c:83`, `:790`); the takes are on the same paths.

| Entry | Boards | Verdict | Reason |
|---|---|---|---|
| `DDARing::Hold [basepri] -> xPortTaskCanBlock` | all | SAFE-GUARDED | `CutHomingMove` (`DDARing.cpp:868`), `FindStopPoint` (`:872`) and `GetEndCoordinates` (`:874`) reach it only through `debugPrintf`, which calls `Platform::DebugMessage` (and its `MutexLocker`, `Platform.cpp:3362`) only with BASEPRI and PRIMASK 0 (`RepRapFirmware.cpp:219`) |
| `DDARing::PauseMoves [basepri] -> xPortTaskCanBlock` | all | SAFE-GUARDED | `FindStopPoint` (`DDARing.cpp:700`), as for `DDARing::Hold` |
| `DDARing::StopMoves [basepri] -> xPortTaskCanBlock` | all | SAFE-GUARDED | `FindStopPoint` (`DDARing.cpp:1101`), as for `DDARing::Hold` |
| `Move::AddLinearSegments [basepri] -> xPortTaskCanBlock` | all | SAFE-PRIMED | `MoveSegment::Allocate` under `Split` (`Move.cpp:2209`) reaches `Tasks::AllocPermanent` only when the free list is empty (`MoveSegment.cpp:19-31`), and `MoveSegment::PrimeFreeList` (`Move.cpp:2178`) fills it before the booster (census.md, SAFE-PRIMED). `GetMallocMutex` asserts on that path in any case (`Tasks.cpp:117`). On MB6HC, `NewSegment` under `ScheduleFirstSegment` (`:2433`) and `Interrupt` (`:2448`) reaches it only through `debugPrintf`, as for `DDARing::Hold` |
| `Move::PhaseStepControlLoop [basepri] -> xPortTaskCanBlock` | MB6HC | SAFE-GUARDED | `UpdateCurrentMotion` (`Move.cpp:2797`) and `Interrupt` (`:2851`) reach it only through `NewSegment` and `debugPrintf`, as for `DDARing::Hold` |
| `Move::UpdateCurrentMotion [primask] -> xPortTaskCanBlock` | MB6HC | SAFE-GUARDED (census.md B2) | `NewSegment` (`Move.cpp:2561`) reaches it only through `debugPrintf`, which tests PRIMASK too (`RepRapFirmware.cpp:219`) |
| `RepRap::StartIap [primask] -> xPortTaskCanBlock` | all | SAFE-GUARDED | `MassStorage::CombineName` (`RepRap.cpp:2280`) reaches it only through `MessageF`, which it calls only when the name overflows (`MassStorage.cpp:228-234`); the `OpenFile` check before excludes that (`RepRap.cpp:2059`), as for `RepRap::StartIap [primask] -> FileStore::Close` |
| `DDA::Prepare [basepri] -> indirect call` | all | SAFE-NO-UNMASK | `CheckEndstops(false)` (`DDA.cpp:1492`). `Move::CheckEndstops` calls through a register the endstop and Z probe overrides of `CheckTriggered` and `Acknowledge` (through `EndstopsManager::CheckEndstops`, `EndstopsManager.cpp:320`, `:340`), `Kinematics::GetHomingMode`, whose overrides return a constant (`src/Movement/Kinematics/*.h`), and `IterateDrivers` with the lambdas of `StopAxisOrExtruder` (`Move.cpp:3378-3389`). The query of every override (`SwitchEndstop`, `GpInPortEndstop`, `StallDetectionEndstop`, `ZProbe`, `ZProbeEndstop`; `ZProbe::Stopped`, `GetReading`, `LocalZProbe::GetRawReading`, `RemoteZProbe::GetRawReading`, `RemoteZProbe::Stopped`, `GpInputPort::GetState`, `FilamentMonitor::GetVirtualInputState`) reaches nothing that lifts a mask or blocks |
| `Move::OnEndstopOrZProbeStatesChanged [basepri] -> indirect call` | all | SAFE-NO-UNMASK | `CheckEndstops(true)` (`Move.cpp:4154`), as for `DDA::Prepare` |
| `DDARing::PauseMoves [basepri] -> indirect call` | all | SAFE-NO-UNMASK | `FindStopPoint` (`DDARing.cpp:700`) reaches only `debugPrintf`, whose `putc` with a mask is `Platform::IsrDebugPutc` (`RepRapFirmware.cpp:217`), a ring buffer write (`Platform.cpp:385-394`) |
| `Move::PhaseStepControlLoop [basepri] -> indirect call` | MB6HC | SAFE-NO-UNMASK | `UpdateCurrentMotion` (`Move.cpp:2797`) reaches `debugPrintf` in `NewSegment`, as for `DDARing::PauseMoves`; `Interrupt` (`:2851`) reaches `CheckEndstops`, as for `DDA::Prepare` |
| `Move::UpdateCurrentMotion [primask] -> indirect call` | MB6HC | SAFE-NO-UNMASK | `NewSegment` (`Move.cpp:2561`) reaches `debugPrintf`, as for `DDARing::PauseMoves` |

Two keys that were there before have new sites with PR #1, and their reasons in `unmask-allowlist.txt` cover them now:
- `DDARing::Hold [basepri] -> indirect call`: `CutHomingMove`, `FindStopPoint` and `GetEndCoordinates` (`DDARing.cpp:868-874`)
  reach the kinematics overrides, which the row `DDARing::Hold [basepri] -> vPortExitCritical` above lists, and `debugPrintf`.
- `Move::AddLinearSegments [basepri] -> indirect call`: `Interrupt` (`Move.cpp:2448`) reaches `CheckEndstops`, as for
  `DDA::Prepare`.
