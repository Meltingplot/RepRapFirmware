# Laser task allocation host test

This test runs RRF's real `Move::CreateLaserTask` and the definitions of `Move::laserTask` and `Move::laserTaskLock` from
`src/Movement/Move.cpp` on a PC.
The Makefile copies their text out of the source at build time, as `Tools/laser-task-test` does, because `Move.cpp` as a whole
needs the hardware headers of CoreN2G. `laser_task_alloc_test.cpp` gives them a minimal `Move`, `TaskBase`, `Task`,
`TaskCriticalSectionLocker`, `ReadWriteLock` and `WriteLocker`, and replaces `::operator new` and `::operator delete` to watch the allocation of the task.

## What is tested and why

`Task<>` has no `operator new` of its own (RRFLibraries `src/RTOSIface/RTOSIface.h`), so the laser task comes from
`::operator new`, and so from RRF's `malloc`, which must not run with the scheduler suspended (`GetMallocMutex` in
`src/Platform/Tasks.cpp`). Up to 3.7-dev de1bfe598, `CreateLaserTask` allocated the task inside its `TaskCriticalSectionLocker`,
which suspends the scheduler. Upstream 5bc928ebf allocated it before the locker. Upstream c37cac98f replaced the locker by a write
lock on `laserTaskLock` around the check, the allocation and the creation, and sets `laserTask` only after `Create`, so that two
callers cannot both find the pointer null and each create a task, and nothing can notify a task that has not been created yet.
M452, M670, a scanning Z probe (M558), M571 and extruder heater feedforward create the laser task.

The test plays the scheduler suspension with a counter, and the write lock with a flag: a task that asks for it while it is held
waits and runs when it is released, as `LockForWriting` does. It checks that the task is allocated with the scheduler running and
that the pointer is not set before `Create`. It also checks what `CreateLaserTask` does: it creates the task once, a second call
does not create another one, and another task that calls it while this one allocates the task (played inside `operator new`) waits
for the write lock and creates no second task.

Against `Move.cpp` of c37cac98f^ with a definition of `laserTaskLock` added (`make SRC=<tree>/src`), every case fails because the
pointer is set before `Create`, and the other task creates a second task.

## Running

```
make check
```
