# M595 DDA allocation host test

This test runs RRF's real `DDARing::ConfigureMovementQueue` (M595) from `src/Movement/DDARing.cpp` on a PC. The Makefile copies
its text out of the source at build time, as `Tools/laser-task-test` does, because `DDARing.cpp` as a whole needs the hardware
headers. `movement_queue_alloc_test.cpp` gives it a minimal `DDARing`, `DDA`, `GCodeBuffer`, `StringRef`, `GCodes` and `RepRap`,
`TaskCriticalSectionLocker` and `Tasks::AllocPermanent`.

## What is tested and why

`DDA::operator new` is `Tasks::AllocPermanent` (`src/Movement/DDA.h`). That takes the malloc mutex through `GetMallocMutex` and
lowers the heap limit (`CoreAllocPermanent` in CoreN2G). `GetMallocMutex` takes the mutex only while the scheduler is running
(`src/Platform/Tasks.cpp`), so an `AllocPermanent` with the scheduler suspended runs without it, while a task that was preempted
inside `malloc` may be about to compare the heap top with the heap limit in `_sbrk`. Up to 3.7-dev de1bfe598, M595 P allocated
each new DDA inside its `TaskCriticalSectionLocker`, which suspends the scheduler.

The test plays the locker with a counter and checks that no DDA is allocated with the scheduler suspended. It also checks what
M595 does: the ring grows to the number of DDAs asked for and stays a consistent ring in both directions, with `addPointer` and the
DDA before it unchanged (the comment in the source says why); a smaller number leaves the ring alone; too little RAM gives an error
and allocates nothing; M595 without P or R reports the settings.

## Running

```
make check
```
