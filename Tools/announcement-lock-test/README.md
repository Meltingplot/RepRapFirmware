# Board announcement lock host test

This test checks that the CAN receiver task does not wait for the SBC task while it holds `ExpansionManager::boardsLock`, when an
expansion board that runs a remote accelerometer collection restarts.

The CAN receiver (`CanReceiverPriority`, `src/Platform/TaskPriorities.h`) handles the announcement of a board in
`ExpansionManager::ProcessAnnouncement` (`src/CAN/ExpansionManager.cpp`) and holds `boardsLock` for writing while it updates the
board's data. If the board restarted, a collection that ran on it is lost, and `Accelerometers::RemoteBoardRestarted`
(`src/Accelerometers/Accelerometers.cpp`) finishes the accelerometer file: it writes a line, truncates the file and closes it. In SBC
mode the file is on the SBC:
- `FileStore::Open` gives a file opened for writing a write buffer, so the write only fills the buffer. The truncate flushes the file and
  truncates it, and the close closes it (`FileStore::Truncate`, `FileStore::Close` in `src/Storage/FileStore.cpp`). Both ask the SBC task
  and wait up to `SbcMaxRequestTime` (3 s, `src/SBC/SbcMessageFormats.h`) for its reply (`SbcInterface::TruncateFile`, `CloseFile` and
  `DoFileOperation` in `src/SBC/SbcInterface.cpp`).
- The SBC task (`SbcPriority`) handles the packets of a transfer in the order DSF sent them, among them the replies to file operations
  (`SbcInterface::ExchangeData`, e.g. `FileTruncateResult`), and sends a new file request only after them. DSF polls
  the live object model (`UpdateService` in DSF), and `boards` and `boards[].drivers` are live values (`ObjectModelEntryFlags::liveNotPanelDue`,
  `src/Platform/RepRap.cpp`, `ExpansionManager.cpp`). The array table entry of `drivers` names `boardsLock`, so the SBC task holds it
  for reading while it reports the array (`ObjectModel::ReportObjectModelArrayAsJson` in `src/ObjectModel/ObjectModel.cpp`).

If a poll is waiting when the CAN receiver finishes the file under the write lock, the SBC task waits for the lock, and the truncate and
the close each run into the timeout. For up to twice `SbcMaxRequestTime` the CAN receiver handles no CAN message, the SBC task makes no
transfer, and the file is not finished.

The fix decides inside the lock that the board restarted and calls `RemoteBoardRestarted` after the lock is released, before the
announcement is acknowledged. `boardsLock` does not protect the accelerometer state: `Accelerometers::StartAccelerometer` (M956) sets
the file without it.

## What the test compiles

The Makefile copies the real text out of the sources at build time (`extract.awk`, with `#line` directives so that messages point at
the real lines):
- from RRFLibraries `src/RTOSIface/RTOSIface.h` and `RTOSIface.cpp`: `ReadWriteLock` with `LockForReading`, `ReleaseReader`,
  `LockForWriting`, `ConditionalLockForWriting`, `ReleaseWriter` and its `LockRecord`, and `ReadLocker` and `WriteLocker`;
- `ExpansionManager::ProcessAnnouncement` (`src/CAN/ExpansionManager.cpp`) and `Accelerometers::RemoteBoardRestarted`
  (`src/Accelerometers/Accelerometers.cpp`).

`announcement_lock_test.cpp` gives them stand-ins:
- one processor for the tasks, as in `Tools/object-lifetime-test`. Each task is a thread, and only one runs at a time. A task runs
  until it blocks, unless a task with a higher priority becomes ready; while the scheduler is suspended the switch waits until it is
  resumed. `TaskBase::Give` and `TakeIndexed` are the task notifications that `ReadWriteLock` waits with. A wait with a timeout, as the
  wait for the reply of the SBC task, expires when no task can run any more. If no task can run and no wait has a timeout, the test
  reports a deadlock and stops;
- the SBC side: a file operation of another task posts the operation, wakes the SBC task and waits for its reply with
  `SbcMaxRequestTime`. The SBC task loops over transfers: it answers a waiting live poll by reporting `boards[].drivers` of the board
  under a read lock on `boardsLock`, then gives the reply of the file operation, then waits for the next transfer. It handles the poll
  before the reply, which is the order in which a poll blocks the reply in the firmware; a reply that DSF sends before the poll in the
  same transfer would get through;
- the CAN message of the announcement (`announceV0`, `announceV1` as in CANlib), the board data, `Event::AddEvent` and the state of the
  accelerometer collection that `RemoteBoardRestarted` uses.

The calls from the copied code into the stand-ins are the points where the other task's work arrives. For each point the test runs
the scenario once with the work arriving there:
- the CAN receiver gets the announcement and the live poll of DSF arrives at one of its points;
- the SBC task answers a live poll and the announcement arrives at one of its points.

Each runs for an announcement of a restarted board in both formats and for one that only regained time sync (`isReconnect`), with and
without a collection running on the board, and with the same or another firmware version, which makes `ProcessAnnouncement` allocate new
driver data. Each run checks that no file operation ran into the timeout, that the file of a collection on a restarted board was
written, truncated and closed and its run recorded, that nothing touched the file otherwise, that the poll was answered with the
board's drivers, that the board data were updated and the announcement acknowledged, that the lock code raised no `RTOS_ASSERT`, and
that `boardsLock` is free at the end.

Without the fix the plain build fails 16 of 88 runs: a restarted board with a collection running, both formats, both firmware versions,
with the poll arriving at `Event::AddEvent`, `FileStore::Write`, `FileStore::Truncate` or `FileStore::Close`, so before or during the
file operations under the write lock.

Not covered:
- the malloc mutex: `ProcessAnnouncement` allocates under the write lock, and the stand-ins take no malloc mutex;
- the real `DoFileOperation`, `WaitForFileSlot` and the asynchronous write of the write buffer: the flush before the truncate is part of
  the one truncate operation here;
- time: the test does not count how long a wait lasts, it only reports that it ran into its timeout.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`), and RRFLibraries from next to it (`RRFLIBS=`).
The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from
Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/announcement-lock-test and build/asan-announcement-lock-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against sources without the fix, e.g. `make clean check RRF=<checkout of the previous commit>`. The generated
files depend on the source files, not on `RRF=`, so `make clean` comes first.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
