# ReadWriteLock lock record host test

This test runs the real `ReadWriteLock` of RRFLibraries (`src/RTOSIface/RTOSIface.cpp`, with `RTOSIface.h` and
`General/FreelistManager.h`), RRF's real `malloc` and `free` (`src/libc/nano-mallocr.c`) with RRF's malloc mutex
(`GetMallocMutex` and `ReleaseMallocMutex` from `src/Platform/Tasks.cpp`), the real `_sbrk` and `CoreAllocPermanent` of CoreN2G
(`src/syscalls.h`), and the real `vTaskSuspendAll` and `xTaskGetSchedulerState` of FreeRTOS (`src/tasks.c`) on a PC. The Makefile
copies the text of the functions out of the sources at build time, with `#line` directives, as `Tools/laser-task-test` does.
`host_rtos.cpp` plays one processor with FreeRTOS's scheduling rules: one thread per task, and only one of them runs at a time. It
also plays the interrupt mask of CoreN2G's `AtomicCriticalSectionLocker`: an interrupt that comes while it is held is taken when it
is released.

RRFLibraries and CoreN2G are separate repositories, so the Makefile reads them from `RRFLIB` and `COREN2G`, by default
`../RRFLibraries` and `../CoreN2G` next to this repository: `make check RRFLIB=<path>` tests another tree, e.g. one with a fix.

## What is tested and why

nano-mallocr.c takes the malloc mutex through `MALLOC_LOCK` (`nano-mallocr.c:134`), which is `GetMallocMutex`. Up to RRF
d85662412 that took the mutex only while the scheduler was running, so a `malloc` with the scheduler suspended ran without it. If
a lower priority task was preempted inside `malloc`, both then worked on the free list or on the heap top at once. Since
d85662412 it takes the mutex whenever the scheduler has started (`Tasks.cpp:115`), and with the scheduler suspended FreeRTOS
asserts (`queue.c:1682`, played in `host_rtos.cpp`), which upstream intends: the board resets instead of corrupting the heap.

`ReadWriteLock` keeps a lock record per task that holds or waits for the lock. The records come from a free list
(`FreelistManager.h`), which takes a new one when it is empty. `LockForReading`, `ConditionalLockForReading`, `LockForWriting`
and `ConditionalLockForWriting` take the record after `RTOSIface::EnterTaskCriticalSection()`, which suspends the scheduler. In
RRFLibraries up to 696a015 (3.7-dev before 888b479) the new record came from `::operator new`, i.e. `malloc`. The fork fix 888b479
allocated it before the critical section, and upstream acefba1, which replaced it in the merge 18a6ddd, left the critical section
to allocate (`NewLockRecord`). Upstream d8708f0, taken in the merge f09f64e, removed `NewLockRecord`: the free list takes a new
item from `CoreAllocPermanent` (`FreelistManager.h:51`), which may be called with the scheduler suspended. Since CoreN2G c9b23d7,
`CoreAllocPermanent` and `_sbrk` each check and move their end of the free memory with interrupts disabled.

The race is played with two tasks at RRF's priorities (`src/Platform/TaskPriorities.h`): MAIN (`SpinPriority`, 1) calls `malloc`,
and an interrupt wakes HEAT (`HeatPriority`, 3) at a point inside that `malloc`. HEAT then takes a read lock, as
`Heat::HeaterTask` does on the sensor list, while the lock record free list is empty. The two preemption points are:
- `sbrk`: inside `_sbrk`, before its update of the heap top (`syscalls.h:59`, the Makefile inserts the call there). Since
  c9b23d7 interrupts are disabled at that point, so the interrupt is taken when `_sbrk` enables them again, while MAIN is still
  inside `malloc` and holds the malloc mutex;
- `split`: while `malloc` splits the first free chunk, before `nano-mallocr.c:326` (the `hook326` build inserts one call there).

Each point is run three ways: `race` as above; `freelist` with a lock record already on the free list, so that HEAT does not
allocate; `outside` with the interrupt after MAIN's `malloc` has returned. The `calls` run has MAIN alone call each of the four
functions with an empty free list, and the paths that need no record (a second read lock, a read lock and a second write lock by
the write lock owner, a conditional write lock that fails).

Every run must end without overlapping blocks or a broken free list (checked against the blocks the test holds, the lock records
from `CoreAllocPermanent` among them), without `vAssertCalled`, without an AddressSanitizer report, and without a call of
`::operator new` while the scheduler is suspended. Against an unfixed RRFLibraries (696a015) every run ends in `vAssertCalled`
from `queue.c:1682`, because the record's `malloc` takes the malloc mutex with the scheduler suspended; also those where no task is
inside `malloc` at that moment. Before RRF d85662412 the same tree ended with `::operator new` called with the scheduler suspended,
and the `race` runs corrupted the heap. Against the previous fix (RRFLibraries be9a000, with `NewLockRecord`) every run passes.

The arena is much larger than the blocks, so the `sbrk` scenario does not show the race between `_sbrk` and `CoreAllocPermanent`
that c9b23d7 closes; Duet3Expansion `Tools/alloc-permanent-lock-test` checks that one with ThreadSanitizer.

## Running

```
make check                                    # plain and ASan builds, every run; RRFLibraries from ../RRFLibraries
make check RRFLIB=/path/to/RRFLibraries       # the same against another RRFLibraries tree, e.g. one with the fix
build/lock-record-alloc-test-orig sbrk race   # one run with its full log
```

## Limits

The test plays the scheduler, so it shows the interleaving that FreeRTOS allows, not that the board takes it at that instruction.
`::operator new` and `::operator delete` are stand-ins that call the renamed nano-mallocr.c functions, as libsupc++'s do on the
board. The arena replaces the board's heap, and `AtomicCriticalSectionLocker` is played (`arena.cpp`, `host_rtos.cpp`).
