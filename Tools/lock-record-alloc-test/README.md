# ReadWriteLock lock record host test

This test runs the real `ReadWriteLock` of RRFLibraries (`src/RTOSIface/RTOSIface.cpp`, with `RTOSIface.h` and
`General/FreelistManager.h`), RRF's real `malloc` and `free` (`src/libc/nano-mallocr.c`) with RRF's malloc mutex
(`GetMallocMutex` and `ReleaseMallocMutex` from `src/Platform/Tasks.cpp`), and the real `vTaskSuspendAll` and
`xTaskGetSchedulerState` of FreeRTOS (`src/tasks.c`) on a PC. The Makefile copies the text of the functions out of the sources at
build time, with `#line` directives, as `Tools/laser-task-test` does. `host_rtos.cpp` plays one processor with FreeRTOS's
scheduling rules: one thread per task, and only one of them runs at a time.

RRFLibraries is a separate repository, so the Makefile reads it from `RRFLIB`, by default `../RRFLibraries` next to this
repository: `make check RRFLIB=<path>` tests another tree, e.g. one with a fix.

## What is tested and why

nano-mallocr.c takes the malloc mutex through `MALLOC_LOCK` (`nano-mallocr.c:134`), which is `GetMallocMutex`. That takes the
mutex only while the scheduler is running (`Tasks.cpp:115`), so a `malloc` with the scheduler suspended runs without it. If a
lower priority task was preempted inside `malloc`, both then work on the free list or on the heap top at once.

`ReadWriteLock` keeps a lock record per task that holds or waits for the lock. The records come from a free list
(`FreelistManager.h`), and when that is empty, from `::operator new`. In RRFLibraries up to 696a015 (3.7-dev),
`LockForReading`, `ConditionalLockForReading`, `LockForWriting` and `ConditionalLockForWriting` allocate the record after
`RTOSIface::EnterTaskCriticalSection()`, which suspends the scheduler. The fix allocates it before and returns it to the free list
after the scheduler is resumed when it is not needed.

The race is played with two tasks at RRF's priorities (`src/Platform/TaskPriorities.h`): MAIN (`SpinPriority`, 1) calls `malloc`,
and an interrupt wakes HEAT (`HeatPriority`, 3) at a point inside that `malloc`. HEAT then takes a read lock, as
`Heat::HeaterTask` does on the sensor list, while the lock record free list is empty. The two preemption points are:
- `sbrk`: inside `_sbrk`, between the load and the store of the heap top (the stand-in in `arena.cpp` has the same order as the
  compiled CoreN2G `_sbrk`);
- `split`: while `malloc` splits the first free chunk, before `nano-mallocr.c:326` (the `hook326` build inserts one call there).

Each point is run three ways: `race` as above; `freelist` with a lock record already on the free list, so that HEAT does not
allocate; `outside` with the interrupt after MAIN's `malloc` has returned. The `calls` run has MAIN alone call each of the four
functions with an empty free list, and the paths that need no record (a second read lock, a read lock and a second write lock by
the write lock owner, a conditional write lock that fails).

Every run must end without overlapping blocks or a broken free list (checked against the blocks the test holds), without
`vAssertCalled`, without an AddressSanitizer report, and without a call of `::operator new` while the scheduler is suspended. The
last check is the invariant of the fix: it fails on every run against an unfixed RRFLibraries, also on those where no task is
inside `malloc` at that moment. Against RRFLibraries 696a015 the `race` runs also corrupt the heap (the plain build ends in
`RTOS_ASSERT(foundOwnRecord)` of `ReleaseReader`, the ASan build reports a use-after-poison).

## Running

```
make check                                    # plain and ASan builds, every run; RRFLibraries from ../RRFLibraries
make check RRFLIB=/path/to/RRFLibraries       # the same against another RRFLibraries tree, e.g. one with the fix
build/lock-record-alloc-test-orig sbrk race   # one run with its full log
```

## Limits

The test plays the scheduler, so it shows the interleaving that FreeRTOS allows, not that the board takes it at that instruction.
`::operator new` and `::operator delete` are stand-ins that call the renamed nano-mallocr.c functions, as libsupc++'s do on the
board. The arena replaces the board's heap, and `_sbrk` is a stand-in with the order of the board's loads and stores.
