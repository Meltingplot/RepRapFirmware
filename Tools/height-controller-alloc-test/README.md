# Height controller task allocation host test

This test runs RRF's real `HeightController::Configure` (M951) from `src/Movement/HeightControl/HeightController.cpp` on a PC. The
Makefile copies its text out of the source at build time, as `Tools/laser-task-test` does, because the file as a whole needs the
hardware headers. `height_controller_alloc_test.cpp` gives it a minimal `HeightController`, `TaskBase`, `Task`, `GCodeBuffer`,
`StringRef` and `TaskCriticalSectionLocker`, and replaces `::operator new` and `::operator delete` to watch the allocation of the
task.

## What is tested and why

`Task<>` has no `operator new` of its own (RRFLibraries `src/RTOSIface/RTOSIface.h`), so the height controller task comes from
`::operator new`, and so from RRF's `malloc`. That takes the malloc mutex only while the scheduler is running (`GetMallocMutex` in
`src/Platform/Tasks.cpp`), so a `malloc` with the scheduler suspended runs without it, and a task that was preempted inside
`malloc` and this one then work on the heap at once. Up to 3.7-dev de1bfe598, `Configure` allocated the task inside its
`TaskCriticalSectionLocker`, which suspends the scheduler. M951 runs in the main task.

The test plays the locker with a counter and checks that the task is allocated with the scheduler running. It also checks what
M951 does: H creates the task once, a second M951 does not create another one, without a sensor no task is created, and M951 without
parameters reports the settings or that the controller is not configured. M951 is only a G-code handler in the main task
(`GCodes2.cpp` -> `Move::ConfigureHeightFollowing`), so no other task can create the task while this one allocates its own;
upstream's version of the fix, which replaced the fork's, does not check for that.

## Running

```
make check
```
