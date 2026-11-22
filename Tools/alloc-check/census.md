# Census of the heap-in-region allowlist

Each of the 114 entries of `allowlist.txt` was checked. For a heap finding: which task runs the region, whether the heap can
really be reached there, and which tasks could be inside `malloc` at that moment. For a call through a register: every target
that the firmware sets, and whether one of them reaches the heap. The verdict per entry is its comment in `allowlist.txt`; this
file has the reasoning and the findings.

- **Basis:** 3.7-dev at de1bfe598 (2026-11-22) with RRFLibraries 3.7-dev 696a015, configurations `Duet3_MB6HC` (50 entries),
  `Duet3_MB6XD` (48) and `Duet3Mini5plus` (90). Line numbers refer to that commit.
- **Counts:** 8 SUSPECT, 1 SAFE-PRIMED, 95 SAFE-NO-HEAP, 10 SAFE-NO-TARGET. 84 of the entries are exception handlers (`[isr]`),
  most of them one per peripheral instance of the same driver.
- **How:** the heap findings were read in the code, each fix has a host test under `Tools/` that fails without it, and the
  fixed firmware was built and checked (the key goes away). The targets of the calls through a register were read by two
  agents, one for the exception handlers and one for the task regions, with file and line for each target.

## Verdicts

| Verdict | Meaning |
|---|---|
| SUSPECT | The heap is reached with the scheduler suspended or interrupts masked. The fix branch is in brackets; the entry stays SUSPECT until it is merged |
| SAFE-PRIMED | The allocation in the region is always served by a free list that is filled just before the region |
| SAFE-NO-HEAP | A call through a register: no target that the firmware sets reaches the heap |
| SAFE-NO-TARGET | A call through a register whose pointer the firmware never sets |

`SAFE-BEFORE-SCHEDULER` (code that runs only before the scheduler starts, where `GetMallocMutex` correctly takes nothing) has no
entry: the check found no region that runs only then.

## Task priorities and who can be inside malloc

From `src/Platform/TaskPriorities.h:12-40`, with preemption and time slicing on (FreeRTOS `FreeRTOSConfig.h:77`, `:84`):

| Priority | Tasks |
|---|---|
| 0 | idle |
| 1 | main (`Tasks.cpp:283`), network (`Network.cpp:519`, created in `Network::Activate`) |
| 2 | SBC |
| 3 | heat, USB |
| 4 | Move, TMC, analog in, height controller |
| 5 | laser, CAN sender, Ethernet |
| 6 | CAN receiver, accelerometer |
| 7 | CAN clock |

`malloc` blocks only when it takes the malloc mutex, before it touches the heap, and nothing inside it waits. So when a task of
priority p suspends the scheduler and allocates, the heap can be in use only by a task of priority p or lower that a tick or an
interrupt preempted inside `malloc`, or by a task of priority p in its time slice. A region in the main task has the network task
as its only such peer. In SBC mode the network is not activated (`RepRap.cpp:639-643`), so a main-task region has no peer there:
the finding is latent until another task at priority 1 exists (`Move::SimulateSteppingDrivers` lowers the Move task to priority 1
while it simulates, `Move.cpp:3283-3284`). A region in the heat, Move or CAN receiver task has the main and SBC tasks as peers in
every mode.

## Heap findings

CHX first: MB6HC in SBC mode, with TOOL1LC, EXP1HCL and SZP expansion boards on CAN.

| Entry | Task(s) | CHX | Fix |
|---|---|---|---|
| `ReadWriteLock::LockForReading`, `LockForWriting` (K23) | every task that takes a lock: heat (sensor list), Move, CAN receiver, main, SBC | **real**: heat (3) against main (1) and SBC (2) inside `malloc` | RRFLibraries `fix/alloc-outside-critical-section` 1e34289, test `Tools/lock-record-alloc-test` on RRF `fix/lock-record-alloc` |
| `Event::AddEventV` | heat (heater faults, `LocalHeater.cpp:1069`), CAN receiver (expansion board events, `CommandProcessor.cpp:764`; `ExpansionManager.cpp:181-187, 616`), main (driver events from `Move::PollOneDriver`, `Move.cpp:3986`, `:4021`, called by `Platform::Spin`, `Platform.cpp:883`; M957 `GCodes3.cpp:1595`, filament errors) | **real**: an event from an expansion board or a heater fault while main or SBC is inside `malloc`, whenever the Event freelist is empty | `fix/event-alloc` |
| `SbcInterface::FillBuffer` | main, SBC mode only | latent (no priority-1 peer in SBC mode); the first code of each channel | `fix/sbc-code-buffer-alloc` |
| `DDARing::ConfigureMovementQueue` (M595 P) | main | latent in SBC mode, real without SBC after `network->Activate` | `fix/movement-queue-alloc` |
| `Move::CreateLaserTask` (M452, M670, M571, scanning probe, feedforward) | main | latent in SBC mode | `fix/laser-task-alloc` |
| `HeightController::Configure` (M951) | main | latent in SBC mode | `fix/height-controller-alloc` |
| `CanInterface::SwitchToExpansionMode` (M954, CAN test mode) | main, CAN receiver | not used on a CHX main board | `fix/expansion-mode-alloc` |
| `Move::AddLinearSegments` [basepri] | Move, CAN receiver in expansion mode | SAFE-PRIMED | upstream cdcd49333 |

Notes on the heap findings:
- **K23, `ReadWriteLock`.** The four lock functions allocate their `LockRecord` after `RTOSIface::EnterTaskCriticalSection()`
  (RRFLibraries `RTOSIface.cpp:235-470`). The record comes from the freelist, and from `::operator new` when that is empty
  (`FreelistManager.h:34-48`). `Tools/lock-record-alloc-test` shows the corruption with the real `nano-mallocr.c`, malloc mutex
  and `vTaskSuspendAll`. The fix gets the record before the scheduler is suspended and gives an unused one back after it is
  resumed. `ConditionalLockForReading` and `ConditionalLockForWriting` are not linked into these three configurations and so have
  no entry; the fix covers them too. After the fix the allocation takes the malloc mutex, which asserts if BASEPRI or PRIMASK is
  set (`Tasks.cpp:117`) and is skipped again if the caller has suspended the scheduler. Built against the fixed RRFLibraries
  (with `LIBRARIES_DIR`), the three configurations have exactly the same entries minus the two `ReadWriteLock` keys, so
  no other region reaches a `ReadWriteLock` through direct calls; `CanInterface::SwitchToExpansionMode` was the one that did
  (through `FilamentMonitor::DeleteAll`), and its fix moves that out of the region. A lock taken behind a call through a
  register would not show; the targets read for the SAFE-NO-HEAP entries below take none.
- **`Event::AddEventV`.** The new event is created inside the `TaskCriticalSectionLocker` (`Event.cpp:41-58`). `Event` uses the
  freelist (`Event.h:40`), so it allocates when no released event is on the list. The fix creates it before the locker and
  releases it after the locker when a similar event is queued.
- **`SbcInterface::FillBuffer`.** `BinaryParser::Put` allocates the buffer of a G-code buffer on its first code
  (`BinaryParser.cpp:37-43`), and `FillBuffer` calls it inside its locker (`SbcInterface.cpp:2245`, `:2287`). The comment above
  `Put` says it may run with the scheduler suspended. The fix leaves the first code of a G-code buffer without a buffer in the
  ring, allocates the buffer after the locker and reads the code on the next call. The key stays after the fix, because the check
  cannot see that the remaining path to `Put`'s allocation only runs when the buffer exists.
- **M595, laser task, M951.** `DDA::operator new` is `Tasks::AllocPermanent` (`DDA.h:104`); `Task<>` has no `operator new` of its
  own, so the tasks come from `::operator new`. The fixes allocate before the locker; the two task fixes check again inside the
  locker and free an unused task after it (`TaskBase::TerminateAndUnlink` does nothing for a task that was never created,
  `RTOSIface.cpp:178-200`).
- **M954.** `GCodes::SwitchToExpansionMode` (`GCodes3.cpp:664-677`) calls `FilamentMonitor::DeleteAll`
  (`FilamentMonitor.cpp:767-778`), which takes a `WriteLocker` and deletes each monitor. Inside the region the write lock also
  allocates a lock record and may wait for a reader, which a task must not do with the scheduler suspended. The fix deletes the
  monitors in `CanInterface::SwitchToExpansionMode` before its locker.
- **`Move::AddLinearSegments`.** The region (`Move.cpp:2168-2218`) splits at most one segment and then breaks out of the loop.
  `MoveSegment::PrimeFreeList` (`MoveSegment.h:208`) puts a segment on the free list just before the booster, and
  `MoveSegment::Allocate` (`MoveSegment.cpp:16-33`) takes it. Only `AddSegment` and `AddLinearSegments` (`Move.cpp:1913`, `:2148`)
  allocate segments; they run in the Move task (`DDA::Prepare`) or, in expansion mode, in the CAN receiver task
  (`Move::AddMoveFromRemote`, `CommandProcessor.cpp:402`), never both, and no ISR allocates (the step ISR only releases segments,
  `MoveSegment.h:294-298`).

## Calls through a register

Each group shares one reason in `allowlist.txt`.

**Exception handlers.**
- **Serial (UART, USART on SAME70; SERCOM on SAME5x).** These call a per-instance function from a table (CoreN2G
  `SAM4S_4E_E70/Serial.cpp:31-61`, `SAME5x_C21/Serial.cpp:174-206`). The targets are the `AsyncSerial` interrupt handlers, the SBC
  SPI handler (`SBC/DataTransfer.cpp:504`), the WiFi SPI handler (`WiFiInterface.cpp:2303`) and `SharedI2CMaster`. The
  `AsyncSerial` callbacks are only `GCodes::CommandEmergencyStop` and the Modbus `GlobalTxEndedCallback` (`AuxDevice.cpp:44-47`).
  All of them use ring buffers, `GiveFromISR` and register writes.
- **Pin interrupts (PIO on SAME70, EIC on SAME5x).** These call `pinCallbacks`/`exintCallbacks`. The targets are the WiFi transfer
  request, `InputMonitor`, the fan tacho, the filament monitors (`Interrupt()` overrides), the LIS INT1 and the DHT data
  transition. They use flags, counters, timer reads and `GiveFromISR`.
- **DMA (XDMAC, DMAC).** The targets are the HSMCI callback (`RepRap.cpp:138`), the TMC51xx/TMC22xx transfer callbacks, the I2C
  receive callback and `SpiDevice::DmaComplete`. They use register writes, the StepTimer reschedule and `GiveFromISR`.
- **Step timer (TC0 on MB6HC/MB6XD, TC2 on Mini5plus, `StepTimer.cpp:416`).** The targets are `Move::TimerCallback`, the Z probe
  modulation, the scanning probe and the TMC timer. The step ISR releases segments to the free list only.
- **CAN (MCAN, CAN).** `CanDevice::Interrupt` calls `rxCallback` (`CanDevice.cpp:1290`, with PRIMASK set), which is
  `StepTimer::CanTimingMessageReceived` on SAME5x and compiled out on SAME70. `txCallback` is null, and its interrupt is enabled
  only when it is set (`CanDevice.cpp:471-475`).
- **SysTick.** `RepRap::Tick` calls `Heat::SwitchOffAllLocalFromISR` only in its stuck-task path, before a reset. `IsLocal()`
  filters out `RemoteHeater`. `LocalHeater::SwitchOff` sets the PWM to 0, apart from B3 below.
- **SAFE-NO-TARGET (10 entries).** The table is never filled:
  - AFEC: no `afec_set_callback` call, and `AnalogIn.cpp:122-123` disables the AFEC interrupts.
  - AES: no `aes_set_callback` call, and `AesEcb.cpp` polls.
  - NVMCTRL: no `flash_register_callback` call.
  - TinyUSB: the only class driver is CDC, whose `sof` and `xfer_isr` are NULL (LibTinyusb `usbd.c:166-167`), and there are no
    app drivers.

**Task regions.**
- **`debugPrintf`** (`FormattedPrinter::Print`): `DDARing::Hold`, `PauseMoves`, `ReleaseHold`, `Move::PushBabyStepping`,
  `AddLinearSegments`, `PhaseStepControlLoop`, `UpdateCurrentMotion`. All of these are under a Move debug flag (Lookahead,
  PrintBadMoves, PrintTransforms) or an error. The output goes to the M111 F ring buffer (`Platform.cpp:385-394`) or to
  `Platform::DebugMessage`; neither allocates. See B1 and B2.
- **`Kinematics::MotorStepsToCartesian`**, called from `DDA::GetEndCoordinates` by `LowPowerOrStallPause`, `DiscardQueuedMotion`
  and `DoAsynchronousPause`. All seven overrides are stack math.
- **`Move::IterateDrivers`** lambdas (`StopMoves`, `DoEmergencyPause`) set flags or call `CanMotion::StopDriverWhen*`, which walk
  lists and write fields.
- **`Move::CheckEndstops`** (`DDA::Prepare`, `OnEndstopOrZProbeStatesChanged`), via the endstop and Z probe overrides, which read
  pins, the ADC or cached states.
- **`DDARing::AddHoldJog`**, via the kinematics overrides used in `DDA::InitStandardMove`. These are math only.
- **`EndstopsManager::HandleRemoteEndstopChange`**: the base no-op or the `SwitchEndstop` field stores.
- **`AnalogIn::TaskLoop`** (Mini5plus): `AveragingFilter::ProcessReading` or `InputMonitor::AnalogInterrupt`, which is a task
  notify.
- **`SoftwareReset`**: `AsyncSerial::canWrite`, the ring buffer space.
- **`RepRap::StartIap`**: `MassStorage::CombineName` calls `MessageF` only when the name overflows, and `Platform::OpenFile` runs
  the same `CombineName` in the check before (`RepRap.cpp:2059`).

## Findings outside this class

The targets above reach no heap. Four of them block where they must not. These are not allocations, so the check does not report
them, and none is fixed here:
- **B1, `debugPrintf` with the scheduler suspended resets the board.** `debugPrintf` (`RepRapFirmware.cpp:211-223`) goes to
  `Platform::DebugMessage` when there is no M111 F buffer and BASEPRI is 0. It does not check the scheduler.
  `DebugMessage` takes a `MutexLocker` (`Platform.cpp:3362`). If another task holds the mutex, or nobody does,
  `xQueueSemaphoreTake` asserts with the scheduler suspended (FreeRTOS `queue.c:1682`).
  - Reachable from `DDARing::ReleaseHold` (`DDARing.cpp:940`), `Move::PushBabyStepping` (`Move.cpp:939`) and
    `DDARing::AddHoldJog` (`DDARing.cpp:1000`).
  - Needs the Lookahead flag plus a lookahead error, or PrintTransforms (`Move.cpp:1302`, every transform).
  - `DDARing.cpp:500-501` says this code "must make no FreeRTOS calls".
- **B2, the same with PRIMASK set** (MB6HC, PrintBadMoves). `DriveMovement::UpdateCurrentMotion` takes an
  `AtomicCriticalSectionLocker` (`DriveMovement.h:329`), and `NewSegment` (`:374`) can print from inside it. `debugPrintf`'s
  test does not see PRIMASK.
- **B3, SysTick stuck-task path with M111 Heat.** `LocalHeater::SwitchOff` calls `MessageF` (`LocalHeater.cpp:258-261`), which
  takes mutexes in an ISR.
- **B4, `RingBuffer::Init`** (RRFLibraries `RingBuffer.h:69-82`, M111 F) frees and reallocates the debug buffer while an ISR may
  write to it.
