# Height controller task allocation host test

This test runs RRF's real `HeightController::Configure` (M951) from `src/Movement/HeightControl/HeightController.cpp` on a PC. The
Makefile copies its text out of the source at build time, as `Tools/laser-task-test` does, because the file as a whole needs the
hardware headers. `height_controller_alloc_test.cpp` gives it a minimal `HeightController`, `TaskBase`, `Task`, `GCodeBuffer`,
`StringRef`, `TaskCriticalSectionLocker`, `ReadWriteLock` and `WriteLocker`, and replaces `::operator new` and `::operator delete` to watch the allocation of the
task.

## What is tested and why

`Task<>` has no `operator new` of its own (RRFLibraries `src/RTOSIface/RTOSIface.h`), so the height controller task comes from
`::operator new`, and so from RRF's `malloc`, which must not run with the scheduler suspended (`GetMallocMutex` in
`src/Platform/Tasks.cpp`). Up to 3.7-dev de1bfe598, `Configure` allocated the task inside its `TaskCriticalSectionLocker`, which
suspends the scheduler. Upstream 5bc928ebf allocated it before the locker. Upstream c37cac98f replaced the locker by a write lock on
`taskLock` around the check, the allocation and the creation, and sets `heightControllerTask` only after `Create`, so that two
callers cannot both find the pointer null and each create a task, and nothing can notify a task that has not been created yet.

The test plays the scheduler suspension with a counter, and the write lock with a flag: a task that asks for it while it is held
waits and runs when it is released, as `LockForWriting` does. It checks that the task is allocated with the scheduler running and
that the pointer is not set before `Create`. It also checks what M951 does: H creates the task once, a second M951 does not create
another one, without a sensor no task is created, M951 without parameters reports the settings or that the controller is not
configured, and another task that runs M951 while this one allocates the task (played inside `operator new`) waits for the write
lock and creates no second task.

Against `HeightController.cpp` of c37cac98f^ (`make SRC=<tree>/src`), every case that creates the task fails because the pointer
is set before `Create`, and the other task creates a second task.

## Running

```
make check
```
