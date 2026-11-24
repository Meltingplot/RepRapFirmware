# SBC wait under lock host test

In SBC mode the SBC task serves the file operations and macro requests of the other tasks, and it answers the requests of DSF. For
those it takes locks of the object model:
- the live poll of DSF reads each object under the read lock that its table entry names (`ObjectModel::ReportObjectModelArrayAsJson`
  in `src/ObjectModel/ObjectModel.cpp`, `GlobalVariables::ReportAsJson` in `src/ObjectModel/GlobalVariables.cpp`);
- `SetVariable` takes `lifetimeLock` for reading, the global variables for writing, and evaluates the expression, which takes
  `Heap::heapLock` for writing for a string (`case SbcRequest::SetVariable` in `src/SBC/SbcInterface.cpp`).

A task that waits for the SBC task (up to `SbcMaxRequestTime`, `src/SBC/SbcMessageFormats.h`) while it holds such a lock stops the SBC
task until its own request has timed out. The rule is therefore: no task waits for the SBC task while it holds a `ReadWriteLock` for
writing. RRFLibraries counts the write locks of each task (`TaskBase::GetCallerWriteLocksHeld`), and `SbcInterface::WaitForFileSlot`
and `GCodeBuffer::RequestMacroFile` report an internal error when the caller holds one. Every file operation of another task passes
`WaitForFileSlot`.

## Sections

1. The count of write locks in the real `ReadWriteLock`: nested locks, a read lock under the own write lock, two locks,
   `ConditionalLockForWriting`, `DowngradeWriter`, and a lock handed over to a waiting task.
2. Why the rule holds: Main checks whether a file exists while DSF has a request pending that needs a lock Main holds. With a write lock
   the request times out and `WaitForFileSlot` reports it. With a read lock it times out only if DSF wants to write, which the check
   does not see (see 4).
3. C1: the CAN receiver finished the accelerometer file of a restarted expansion board under `ExpansionManager::boardsLock`
   (`ExpansionManager::ProcessAnnouncement`, `Accelerometers::RemoteBoardRestarted`), while the live poll reports `boards[].drivers`
   under that lock.
4. C5: `fileexists()` of a string on the heap held `Heap::heapLock` for reading across the SBC wait. That is a read lock, so the count does
   not cover it; `Platform::SysFileExists(const StringHandle&)` releases the lock first.
5. C2/C3: in SBC mode DSF evaluates `fileexists()` and `fileread()` in the codes it sends. Text that RRF parses itself (USB, PanelDue,
   trigger actions) called them while command handlers or `global`/`set global.` held write locks. Such a command now goes to DSF like
   other codes that DSF handles. The test covers the scanner (`StringParser::CallsFileFunction`) and the hand-over of `global` and
   `set global.` in `StringParser::ProcessConditionalGCode`. The hand-over of G, M and T commands in `GCodes::ActOnCode` and the check in
   `GCodeBuffer::RequestMacroFile` are not covered here.

## What the test compiles

The Makefile copies the real text out of the sources at build time (`extract.awk`, with `#line` directives so that messages point at
the real lines):
- RRFLibraries `src/RTOSIface/RTOSIface.h` and `RTOSIface.cpp`: `ReadWriteLock` and its `LockRecord`, `ReadLocker`, `WriteLocker`,
  `ReadLockedPointer`;
- `src/SBC/SbcInterface.cpp`: `FileExists`, `TruncateFile`, `CloseFile`, `WaitForFileSlot`, `PostFileOperation`, `DoFileOperation`;
- `src/CAN/ExpansionManager.cpp`: `ProcessAnnouncement`; `src/Accelerometers/Accelerometers.cpp`: `RemoteBoardRestarted`;
- `src/Platform/Platform.cpp`: both `SysFileExists`;
- `src/GCodes/GCodeBuffer/StringParser.cpp`: `ProcessConditionalGCode`, `CallsFileFunction`, `SendGlobalAssignmentToSbc`.

`sbc_wait_lock_test.cpp` gives them stand-ins. The tasks run on one simulated processor, as in `Tools/object-lifetime-test`: each task
is a thread, only one runs at a time, and a task runs until it blocks unless a task with a higher priority becomes ready (priorities from
`src/Platform/TaskPriorities.h`). A wait with a timeout expires when no task can run any more. The SBC task first answers the pending
requests of DSF under their locks, then serves the file operation (`SbcInterface::ExchangeData` handles the packets of DSF before it
sends new file requests).

## Running

    make check

builds the test plain and with AddressSanitizer and UBSan and runs both (g++ 13, WSL). `RRFLIBS` points at RRFLibraries, by default
`../RRFLibraries` next to this repository.

Without the fixes the test fails: without the count in RRFLibraries 8 cases, without the check in `WaitForFileSlot` 2, with
`RemoteBoardRestarted` back under `boardsLock` 4, with the heap lock held across the check 1, and without the hand-over of `global` and
`set global.` 3.
