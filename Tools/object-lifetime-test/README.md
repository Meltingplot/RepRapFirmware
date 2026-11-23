# Object model lifetime host test

This test checks that an object model object stays intact while a task other than the main task still uses the result of an
expression that points into it.

The object model holds the lock of a table only while it fetches one value (`ObjectModel::GetObjectValueUsingTableNumber` in
`src/ObjectModel/ObjectModel.cpp`). An expression that ends at an object model array, e.g. `move.kinematics.forwardMatrix` or
`tools[0].offsets`, returns an `ExpressionValue` with a bare pointer to the object that owns the array (`omVal`), and the caller
reads the array through that pointer later, when no lock is held any more. Two kinds of task do that while the main task may delete
object model objects:
- the SBC task (`SbcPriority`, above `SpinPriority` of the main task, `src/Platform/TaskPriorities.h`), for the `EvaluateExpression`
  and `SetVariable` requests of DSF (`SbcInterface::ExchangeData` in `src/SBC/SbcInterface.cpp`). `SetVariable` stores the value in
  a new variable, which needs `malloc` (`VariableSet::InsertNew`, `Variable::CopyObjectModelArray` in `src/ObjectModel/Variable.cpp`),
  and then reports it as JSON. If the main task holds the malloc mutex at that moment, e.g. while M669 allocates the new kinematics in
  `Move::SetKinematics`, the SBC task waits, the main task inherits its priority, keeps it because it still holds the mutex of its
  G-code channel, and goes on to delete the old kinematics before the SBC task runs again. `EvaluateExpression` needs no `malloc`, but
  the same happens when a task with a higher priority waits for the malloc mutex while the main task holds it: the CAN receiver
  (`CanReceiverPriority`) allocates the data of an expansion board that announces itself (`ExpansionManager::ProcessAnnouncement` in
  `src/CAN/ExpansionManager.cpp`), so the main task inherits that priority and runs before the SBC task, which still holds the result;
- without an SBC, the network task (`SpinPriority`, the same as the main task, `src/Networking/Network.cpp`), for the `;CustomInfo`
  lines of `rr_fileinfo` (`FileInfoParser::ProcessCustomInfo` in `src/Storage/FileInfoParser.cpp`). FreeRTOS shares the processor
  between tasks of the same priority at each tick (`configUSE_TIME_SLICING`), so M563 can delete the tool between the parse and the
  copy into the variable.

The fix is one lock on the lifetime of object model objects, `ObjectModel::lifetimeLock`. These readers hold it for reading from the
parse until they have finished with the result, and the main task holds it for writing while it deletes an object model object,
taken before the lock of the list or array that holds the object. A deleting command therefore waits for a running SBC or HTTP
evaluation.

## What the test compiles

The Makefile copies the real text out of the sources at build time (`extract.awk`, with `#line` directives so that messages point at
the real lines):
- from RRFLibraries `src/RTOSIface/RTOSIface.h` and `RTOSIface.cpp`: `ReadWriteLock` with `LockForReading`, `ReleaseReader`,
  `LockForWriting`, `ReleaseWriter` and its `LockRecord`, and `ReadLocker`, `WriteLocker` and `WriteLockedPointer`;
- `Move::SetKinematics` (`src/Movement/Move.cpp`) and `Tool::DeleteTool` (`src/Tools/Tool.cpp`), the deleting commands;
- the `EvaluateExpression` and `SetVariable` cases of `SbcInterface::ExchangeData` and `FileInfoParser::ProcessCustomInfo`, the
  readers.

`object_lifetime_test.cpp` gives them stand-ins:
- one processor for the tasks. Each task is a thread, and only one runs at a time. A task runs until it blocks, unless a task with a
  higher priority becomes ready; while the scheduler is suspended (`RTOSIface::EnterTaskCriticalSection`) the switch waits until it
  is resumed. `TaskBase::Give` and `TakeIndexed` are the task notifications that `ReadWriteLock` waits with. `Mutex` is recursive and
  passes on the priority of a waiting task to its holder, which gives it back only when it holds no other mutex, as FreeRTOS does.
  `malloc` and `free` take a malloc mutex while the scheduler runs, as `__malloc_lock` in `src/Platform/Tasks.cpp` does. If no task
  can run while some are blocked, the test reports a deadlock and stops;
- the object model as far as these readers use it: an object that owns an array of three numbers, `ReportItemAsJsonFull` and
  `CopyObjectModelArray` for that array, and an expression parser for the two expressions above. Like the real parser it holds
  `Tool::toolListLock` only while it fetches `tools[0]` (`RepRap.cpp` names that lock for the `tools` array), and no lock while it
  reads `move.kinematics` (`OBJECT_MODEL_FUNC(self->kinematics)` in `Move.cpp`);
- variables (`VariableSet`, `GlobalVariables`), the transfer to DSF, and what the copied functions call on `reprap`, the G-code
  buffers, the heaters and the tools.

The calls from the copied code into the stand-ins are the points where another task can get the processor. For each point the test
runs the scenario once with the other task getting the processor there:
- SBC mode, M669 replaces the kinematics. The main task holds the mutex of its channel, as `GCodes::SpinGCodeBuffer` does, and runs
  the real `Move::SetKinematics`. At one of its points the SBC task gets a request from DSF: `SetVariable` for
  `global k = move.kinematics.forwardMatrix`, or `EvaluateExpression` for `echo move.kinematics.forwardMatrix`. Each of these runs
  again with a third task: right after the SBC task has read `move.kinematics`, a board announcement wakes the CAN receiver, which
  allocates memory;
- standalone, M563 deletes tool 0 while the network task reads `;CustomInfo t0 = tools[0].offsets`. Either task starts, and at one of
  its points the time slice gives the processor to the other one.

Each run checks that no task uses an object after the main task has deleted it, that the tasks do not deadlock, that the lock code
raises no `RTOS_ASSERT`, that M669 and M563 delete the old object exactly once, and that DSF or the variable gets the values of the
old or the new object, and nothing else.

Each object records in a field whether its destructor has run, and the readers check that field before they read the array. The
plain build keeps freed memory, so that check reports the use. The AddressSanitizer build frees it, so the check itself is the read of
freed memory, and ASan stops the test with a `heap-use-after-free` report that shows the reader and the deleting command.

Without the fix the plain build fails seven runs: `SetVariable` arriving while M669 allocates the new kinematics, with and without the
board announcement, `EvaluateExpression` arriving then with the announcement, and the network task losing the processor after it has
fetched `tools[0]`. The ASan build stops with a heap-use-after-free in the `SetVariable` case (`VariableSet::InsertNew`), freed by
`Move::SetKinematics`.

Not covered:
- time slicing between two tasks that have the same priority because one has inherited it, e.g. the main task and the CAN receiver;
  the test switches between them only when one blocks or gives back an inherited priority;
- the stand-in allocates lock records with `malloc` directly, not through `FreelistManager`; since RRFLibraries 1e34289 the real
  lock allocates them before it suspends the scheduler, as the copied code does.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`), and RRFLibraries from next to it (`RRFLIBS=`).
The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it from
Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/object-lifetime-test and build/asan-object-lifetime-test (AddressSanitizer + UBSan), then run both

To see the failure, run it against sources without the fix, e.g. `make check RRF=<checkout of the previous commit>`.

Each run prints `PASS` or `FAIL` with the reason. The exit code is 1 if any run fails.
