# Laser task allocation host test

This test runs RRF's real `Move::CreateLaserTask` and the definition of `Move::laserTask` from `src/Movement/Move.cpp` on a PC.
The Makefile copies their text out of the source at build time, as `Tools/laser-task-test` does, because `Move.cpp` as a whole
needs the hardware headers of CoreN2G. `laser_task_alloc_test.cpp` gives them a minimal `Move`, `TaskBase`, `Task` and
`TaskCriticalSectionLocker`, and replaces `::operator new` and `::operator delete` to watch the allocation of the task.

## What is tested and why

`Task<>` has no `operator new` of its own (RRFLibraries `src/RTOSIface/RTOSIface.h`), so the laser task comes from
`::operator new`, and so from RRF's `malloc`. That takes the malloc mutex only while the scheduler is running (`GetMallocMutex` in
`src/Platform/Tasks.cpp`), so a `malloc` with the scheduler suspended runs without it, and a task that was preempted inside
`malloc` and this one then work on the heap at once. Up to 3.7-dev de1bfe598, `CreateLaserTask` allocated the task inside its
`TaskCriticalSectionLocker`, which suspends the scheduler. It runs in the main task: M452, M670, a scanning Z probe, M571 and
extruder heater feedforward create the laser task.

The test plays the locker with a counter and checks that the task is allocated, and an unused one freed, with the scheduler
running. It also checks what `CreateLaserTask` does: it creates the task once, a second call does not create another one, and if
another task creates the laser task while this one allocates its own, only one task is created and the second allocation is
freed. The other task is played inside `operator new`, and only while the scheduler is running, because only then can the board
switch tasks there.

## Running

```
make check
```
