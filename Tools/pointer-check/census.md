# Census of the raw pointer allowlist

Each of the 272 entries of `allowlist.txt` was checked: who writes the pointer, who reads it, from which task or ISR and at
which priority, under which lock, and whether the object it names can be freed while a reader still holds it. The bug class
is the one of #50 and #51: one context frees an object while another context can still reach it. The result per entry is
the comment in `allowlist.txt`; this file has the findings and the reasoning behind them.

- **Basis:** 3.7-dev at e5b7878a1 (2026-11-21). Line numbers refer to that commit unless a line names another one. They are
  the same at 9a585f644 except in `ExpansionManager.cpp`.
- **Upstream:** Duet3D `3.7-dev` a507cb8f0 and `3.7-ch` eed7cf1fc. "Fixed upstream 43a6b4fa8" means that commit fixes the
  case. It came into the fork with #49.
- **Merge of 3.7-dev at 6793b3064:** #49 added five entries and renamed one: `DDARing::holdBarrier`, `lastHomingMoveDda`
  (e7e30e13f), `returnMoveDda` (7e9556797), `BufferedStreamGCodeInput::directBuffers`, `directTail` (eed7cf1fc), and
  `CanMotion::StopListState::stopList` (43a6b4fa8, was `CanMotion::stopList`). These were checked in that commit. The
  entries whose finding is fixed there (#54, #57 and 43a6b4fa8) carry the verdict of the fixed code.
- **How:** the entries were split by subsystem into six groups, each of which an agent read in the code. Each finding that got
  a fix, and K16 and K17, was then read again in the code, and each fix has a host test under `Tools/` that fails without it.
  The other findings (K14, K18 to K21, the lists without an SBC and the minor ones) come from that first reading.

## Task priorities

From `src/Platform/TaskPriorities.h`, with preemption and time slicing on (FreeRTOS `FreeRTOSConfig.h:77`, `:84`):

| Priority | Tasks |
|---|---|
| 0 | idle |
| 1 | main (`SpinPriority`), and the network task, created in `Network::Activate` (`Network.cpp:473-519`) |
| 2 | SBC |
| 3 | heat, USB |
| 4 | Move, TMC, analog in, height |
| 5 | laser, CAN sender, Ethernet |
| 6 | CAN receiver, accelerometer |
| 7 | CAN clock |

In SBC mode the network task is not activated (`RepRap.cpp:637-638`), and the SBC task serves the object model. Without an
SBC, `config.g` runs before `network->Activate` (`RepRap.cpp:594-597` vs `:645`).

## When does reseating before freeing protect a reader?

A lock-free reader is safe when the writer reseats the pointer before it frees the object, and **every** reader
- has a higher priority than the writer, so the writer never runs between the reader's load and its last use, and
- does not block between the load and its last use.

Under these conditions the reader sees either the old object, which is intact until the reader has finished with it, or the
new one. The argument fails in three cases:
- **Same priority.** The network task runs at the main task's priority, with time slicing. A tick can switch from it to the
  main task at any instruction, so the order fixes of #50 and #51 protect the SBC task (2), not the network task (1).
- **Reader below the writer.** The SBC task (2), the CAN receiver (6) or an ISR frees, and the main task (1) reads.
- **A reader that blocks.** `ReadWriteLock::LockForReading` blocks while a writer holds the lock and also while one waits
  for it (RRFLibraries `RTOSIface.cpp:264-279`). So `StringHandle::Get` (`StringHandle.cpp:104`) and every `ReadLocker` in an
  object model walk are points where the writer can run.

Facts that several entries share:
- **Copying a handle takes two steps without a lock.** The copy reads `slotPtr`, then increments `refCount`
  (`StringHandle.cpp:131-135`, `ObjectModel.cpp:276-292`, `Heap.h:50-53`). A reader at the same or a lower priority can be
  switched out between the two. The writer then frees the slot (`Heap.cpp:377-382`), and the reader increments a free slot:
  its reference count is 1 and its storage is null. The next `Get` passes `CheckSlotGood` (`Heap.cpp:366`) and reads through
  null storage, and the release fails `RRF_ASSERT(slotPtr->storage != nullptr)` (`Heap.cpp:376`), which resets the board. If
  the slot was reused meanwhile, the reader holds a reference to another string.
- **Free lists overwrite the first word.** `GCodeMachineState`, `BlockState`, `MessageBox`, `LinkedVariable`, `Event` and the
  Z probe classes use free-list new and delete. Release writes the link into the first word (RRFLibraries
  `FreelistManager.h:50-56`). For a class with virtual functions, that word is the vtable pointer.

## Result

| Verdict | Entries | Meaning |
|---|---|---|
| SAFE-PERMANENT | 115 | set once at start-up and never freed |
| SAFE-ONE-TASK | 59 | read and written in one task only |
| SAFE-LOCKED | 46 | the other tasks hold the lock that the writer holds |
| SAFE-ORDER | 16 | the conditions above hold |
| SAFE-NO-DEREF | 1 | never dereferenced (`Heat::lastStandbyTools`) |
| SUSPECT | 35 | a reader can reach memory that is no longer valid |

The pointers into the DDA ring and into the direct USB buffers count as SAFE-PERMANENT, because their targets are never
freed. An entry whose finding is fixed in 3.7-dev names the fix in its comment. The SUSPECT entries name their finding.

## Findings that apply with an SBC

These can happen on a Duet 3 with an SBC and expansion boards. Each fix is its own PR, with a host test that fails without
the fix. "Fixed by" means the fix is in 3.7-dev; a PR number alone is a fix PR that is still open.

| | Finding | Status |
|---|---|---|
| K1 | **Driver data freed under M569.** The CAN receiver (6) replaces `driverData` under the write lock of `boardsLock` when a board announces another firmware (`ExpansionManager.cpp:170`, `:221`). The M569 accessors in the main task read it without a lock (`:411-438`). | fixed by #54 |
| K2 | **The SBC task deletes a stack level that the main task reads.** On `MacroCompleted` with an error, the SBC task (2) ran `CloseFile`, `PopState` and `Init` (`SbcInterface.cpp:624-631`). `PopState` deletes the level (`GCodeBuffer.cpp:1168-1169`). The main task walks the stack without that mutex in M122 (`GCodeBuffer.cpp:265-276`) and for `inputs[]` (`:58-78`). The writer outranks the reader, so the order does not help. | #58: the main task pops, the SBC task only records it |
| K3 | **M558 and M502 delete a Z probe that is still in the active list.** Only a trigger takes a probe out of `activeEndstops` (`EndstopsManager.cpp:323`). A remote input change walks the list at any time (`CommandProcessor.cpp:209/251` → `Move.cpp:4038-4046`). M574 clears the list first (`:475`); M558 (`:757`) and M502 (`:628-635`) did not. | #56; its standstill wait is upstream's, from 43a6b4fa8 |
| K4 | **A file write overflows the transmit buffer at exactly 8 free bytes.** `WriteFileData` (`DataTransfer.cpp:2219`): `freeSpace - 8 - 4` wraps. This is not a pointer entry; it turned up in the SBC group. | fixed by #55 |
| K5 | **Variables keep pointers to object model objects.** `Variable::Assign` rejected a single object (`Variable.cpp:85-86`), but copied the elements of an object model array as raw `ObjectModel *` (`:57-83`). They dangle after M950, M308 or M563. | fixed by #57 |
| K6 | **Variables keep pointers into the object model.** A string (CString) or a port was stored as a raw pointer (`Variable.cpp:91`), e.g. `state.messageBox.message` or `move.extruders[].filament`. On the CHX, `daemon.g` compared the filament with a variable that followed the current name. | fixed by #57 |
| K7 | **Error messages point into a dead stack frame.** `GCodeException::message` must be a literal (`GCodeException.h:24`), but `Heater.cpp:644` and `SwitchEndstop.cpp:155` threw with a local buffer. | #61 |
| K8 | **Message box expressions read before the lock.** The expression path calls the lambda before it takes the table lock (`ObjectModel.cpp:1302` vs `:1304`). | fixed upstream 43a6b4fa8 |
| K9 | **A single global is read without its lock.** `M409 K"global.x"` takes no read lock (`GlobalVariables.cpp:46-63`) while the SBC task's `SetVariable` rewrites globals (`SbcInterface.cpp:1068`, `:1156`). | fixed upstream 43a6b4fa8 |
| K10 | **A file request that the end of the connection woke returned the result before.** `InvalidateResources` gave the semaphore without failing the request (`SbcInterface.cpp:1891-1903`). `ReadFile` returned the old offset as a byte count (`:2487`), and `ReadLine` wrote past its buffer. | #60 |
| K11 | **A deleted Z probe stays in the step timer list.** M672 schedules the probe's timer (`LocalZProbe.cpp:153-172`), whose destructor did not cancel it (`:23-27`). M558 or M502 during the send left freed memory in `StepTimer::pendingList`. | #63 |
| K12 | **Delete before clearing the pointer.** The M574 P path (`EndstopsManager.cpp:502-503`) and `Move::Exit` (`Move.cpp:670-671`). For `laserTask`, the reachable case is a second emergency stop that preempts the first, which deletes the task again. | M574 fixed upstream 43a6b4fa8; `laserTask` #62 |
| K13 | **Two tasks write the event log.** `inLogger` was a plain bool (`Logger.cpp:110`, `:117`). | fixed upstream 43a6b4fa8 |
| K15 | **Resend requests take the transmit free space below zero.** `FreeTxSpace` kept the room for resend requests (`DataTransfer.h:229-232`), but `ResendPacket` writes into it without a check (`:239-242`). After an exact fit the value wrapped, and later writes of the transfer went past the buffer. | fixed by #59 |
| K16 | **A late MacroCompleted with error pops the caller's level.** After the 3 s timeout of `RequestMacroFile` (`GCodeBuffer.cpp:1324-1333`), no level is pushed, but a late error from DSF still pops whatever is on top (`SbcInterface.cpp:610-631`). | SBC link v8 design (DuetSoftwareFramework #201) |
| K17 | **A stale entry of the active endstop list stops an unrelated move.** A probe or endstop stays linked after a move without a trigger. `Move::OnEndstopOrZProbeStatesChanged` checks the list without checking that a move monitors endstops (`Move.cpp:4038-4046`), so a later trigger stops whatever moves. 3.5.4 had that check (`EndstopsManager.cpp:823`). The work-in-progress commit 73f2b4196 (2023-12-19) removed it without a stated reason. The per-segment flag `checkEndstops` came later (d4b264c2e, 2024-04-25); the step ISR checks it for the due segments (`Move.cpp:3030-3041`), but this path never got such a check. | open, question for upstream |
| K18 | **The return value of `WriteLocked` is ignored** (`SbcInterface.cpp:689`). Without room, the Locked answer is lost while the movement lock is held. | open (DuetSoftwareFramework #201) |
| K19 | **A late `OpenFileResult` overwrites `fileOffset`** in any state (`SbcInterface.cpp:1292`). | open (DuetSoftwareFramework #201) |

## Low, not on the CHX

| | Finding | Status |
|---|---|---|
| K14 | `DDA::tool` (`DDARing.cpp:698`, `:858`) and `DDARing::lastFeedForwardTool` (laser task) can outlive a tool that M563 deletes at run time. `StringParser` reads the CRC after `Close` (`StringParser.cpp:1921-1922`), where another task may already have reused the slot. The accelerometer entries need a local LIS accelerometer. | report only |
| K20 | `RepRap::EmergencyStop` has no guard against a second call (`RepRap.cpp:1049-1104`). The SBC task can preempt the main task's emergency stop, so `ring.Exit`, `heat->Exit` and the rest run twice at once. #62 only closes the `laserTask` part. | report only |
| K21 | `StepTimer::ScheduleCallbackFromIsr` (`StepTimer.cpp:508-535`) queues a timer behind one due at the same tick and returns false. Two Z probe program sends due at the same tick then loop in `StepTimer::Interrupt`. This was seen with an instrumented copy on the host. | report only |

## Without an SBC only

The network task runs at the main task's priority (see above). Each of the following needs a request from the network task
while the main task frees the object at run time, so it happens rarely. None of them can happen in SBC mode. They are reported
here with no fork fix.

- **Kinematics.** `rr_model` walks `move.kinematics` (`HttpResponder.cpp:528-533`, `Move.cpp:185`). A tick switches to the main
  task, whose M669 with another type deletes the old object (`Move.cpp:951-953`). The network task resumes with virtual calls
  on freed memory. The order fix of #51 does not cover this.
- **Handle copies** (`StringHandle::slotPtr`, `ArrayHandle::slotPtr`). The network task copies `inputs[].currentFile` or a global
  array and is switched out between reading `slotPtr` and the increment. The main task pops the level or reassigns the global,
  and the slot is freed (see the facts above). The symptom is the reset of #50. The lambdas that call `IncreaseRefCount` and then
  copy (`ObjectTracker.cpp:57`, `RepRap.cpp:417/419`) read `slotPtr` twice.
- **G-code stack** (`GCodeBuffer::machineState`). The network task walks `inputs[]` while the main task pops a level.
- **Power fail script** (`GCodes::powerFailScript`). The network task copies the old script (`RepRap.cpp:364`) while M911 replaces
  it (`GCodes2.cpp:4618-4621`).
- **Display** (`Display::lcd`, `Display::encoder`, Mini 5+). The network task reports `boards` with virtual calls (`Lcd.cpp:20-23`)
  while M918 deletes the display (`Display.cpp:217`).
- **Z probes** (`EndstopsManager::zProbes`). M502 deletes the probes without `zProbesLock` (`EndstopsManager.cpp:633`) while the
  network task serialises `sensors.probes[]` under a read lock. `gpIn[].value` of a probe reads it without a lock
  (`GpInPort.cpp:43-47`).
- **Current tool** (`MovementState::currentTool`). M563 deletes the tool (`Tool.cpp:289-313`) while the network task reads its
  number (`RawMove.cpp:375`). The value is one stale int.
- **Job file** (`FileData::f`). The network task reads `job.filePosition` (`StringParser.cpp:1246-1253`) while the main task
  closes the print file (`FileData.h:65-68`). The slot is pooled, so the value is wrong but nothing is freed.
- **MQTT strings** (`MqttClientConfig::id` and the other strings). The network task copies them in `Accept` (`MqttClient.cpp:240-243`)
  while M586 P4 S0 and M586.4 delete them (`:322`).
- **lwIP sockets.**
  - `LwipSocket::receivedData`: the network task holds a pbuf without `lwipMutex` (`LwipSocket.cpp:360-370`) while the Ethernet
    task (5) frees it on a reset from the peer, or the main task terminates the socket (M586).
  - `LwipSocket::connectionPcb`: `UsingTls` dereferences the pcb without the mutex (`:95`).
  - `LwipEthernetInterface::tlsConfig`: after M552 S0, M586 P0 S0 and M552 S1, a TLS listener stays open while a later
    `ShutdownProtocol` frees its configuration (`LwipEthernetInterface.cpp:406`).
- **WiFi buffers** (`WiFiSocket::receivedData`, `NetworkBuffer::freelist`). M586 terminates sockets and releases their buffers
  (`WiFiInterface.cpp:376-404`, `NetworkBuffer.cpp:23-29`) while the network task allocates or reads them without
  `interfaceMutex`.
- **GMAC restart** (`gmac_device::rx_pbuf`). After a PHY failure, a second M552 S1 re-initialises lwIP and the receive ring in
  the main task while the Ethernet task still runs on the old pbufs (`LwipEthernetInterface.cpp:686-707`).

## Minor, not memory safety

- `LocalLedStrip::chunkBuffer` (Mini 5+): M950 deletes a strip while DMA still reads its buffer. That gives wrong LED colours,
  no corruption.
- `new[]` freed with `delete`: `LocalLedStrip.cpp:53`, `MonoLcd.cpp:24`, `MqttClient.cpp:322` (the last one is fixed upstream
  43a6b4fa8).
- Fixed upstream 43a6b4fa8:
  - the null dereference in an MQTT debug print (`MqttClient.cpp:122-125`);
  - the missing null check in `HandleM950Led` (`LedStripManager.cpp:257`);
  - the missing null check of `zProbes[0]` in `SetZProbeDefaults` (`:630`).

## For the pointer classes

The findings fall under three rules:
- **R1.** Never delete an object that is still published.
- **R2.** Publish an object only when it is fully built.
- **R3.** Share a pointer without a lock only when the writer has a lower priority than every reader.

Most SUSPECT entries break R1 or R3; K5 and K6 stored a published pointer where no rule can follow it. The pointer class will
build on upstream's `DeleteObject` and `ReplaceObject` (`RepRapFirmware.h:574-595`), which clear the pointer before they
delete, and add the checks of R1 and R3.
