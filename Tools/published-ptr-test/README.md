# PublishedPtr host test

This test runs RRF's pointer class `PublishedPtr` on a PC: the real `src/Platform/PublishedPtr.h` and `src/Platform/PublishedPtr.cpp`,
compiled unchanged, with the real `ReadWriteLock` of RRFLibraries. The Makefile copies the `ReadWriteLock` part of
`RRFLibraries/src/RTOSIface/RTOSIface.cpp` (from "Class ReadWriteLock members" to "// End") at build time, with a `#line` directive
that points at the real lines; the build fails if the part is missing. `RTOSIface.h`, `General/FreelistManager.h` and
`src/Platform/TaskPriorities.h` are the real headers too.

`Move::kinematics` and `GCodeBuffer::machineState` are `PublishedPtr`s. The main task (`TaskPriority::SpinPriority`) is the writer of
both: it replaces the kinematics (M665, M669) and pushes and pops the machine state stack. Other tasks and ISRs read them:
- the Move task and the step ISR read the kinematics, the SBC task serialises both in the object model. They outrank the main task, so
  the main task cannot run while they use the object, and they read without the lock;
- the network task without an SBC (`Network.cpp:519`) has the priority of the main task, and time slicing is on, so the main task can
  run in the middle of its read. It must hold the lock of the `PublishedPtr` for reading: the object model takes it through
  `Move::GetObjectLock` and `GCodeBuffer::GetObjectLock`.

`shim/HostRtos.h` stands in for the parts of FreeRTOS that `RTOSIface.h`, the lock and `PublishedPtr.cpp` use, and `host_rtos.cpp`
implements them on `std::thread`: a thread plays a task, task notifications block, the critical sections and `vTaskSuspendAll` take
one recursive mutex, and BASEPRI, PRIMASK and "in an ISR" belong to the calling thread. As on the board, leaving a critical section
sets BASEPRI to 0, and every priority query goes through one. `shim/RepRapFirmware.h` makes `RRF_ASSERT` record the failure instead
of resetting the processor, and so does `rwlock.cpp` for `RTOS_ASSERT`. A deleted test object keeps its memory until the end, so a
reader that still holds it reads defined memory and the test can report it.

Cases:
1. R1: deleting an object while a `PublishedPtr` points to it fails the `RRF_ASSERT` of `~Publishable`. Without it the SBC task gets the
   deleted object.
2. `Replace` points to the new object first, then deletes the old one once, after it has released the write lock. `Delete` deletes
   once and leaves the pointer empty. `Publish` hands back the old object intact.
3. A `Replace` by another task than the writer fails the writer check.
4. R3: the writer, an ISR, a task while the scheduler is suspended (urgent M24 and M25.1 under `TaskCriticalSectionLocker`) or while
   interrupts are masked, the SBC task and the Move task read without the lock. The writer, ISR, suspended and masked cases query no
   priority, and the masked case leaves BASEPRI set. The SBC task also passes while the main task has inherited a higher priority.
5. R3: the network task, also while it has inherited a higher priority, or a task below the main task's priority, reading without
   the lock fails `CheckHasReadOrWriteLock`. Without
   the check the main task then deletes the object that the network task holds. With its own read lock the network task passes;
   a read lock that another task holds does not count.
6. A machine state stack: two pushes with `Publish`, two pops with `Replace`, a fork that publishes a new chain and deletes the old
   one level by level; every level is deleted once and the readers find intact levels.
7. Two threads: the main task's `Replace` waits while the network task holds the read lock. Meanwhile the old object is intact and
   still published; the network task finds its object intact until it releases the lock, and then the old object is deleted once.

`make nochecks` builds the test with `PUBLISHED_PTR_TEST_NO_CHECKS`, which takes out `RRF_ASSERT` and `RTOS_ASSERT`. Then the six cases
that depend on them (R1, the writer check, and the four R3 reads without the lock) must fail and all others must pass; that build
exits 0 only then.

What the test does not show: the real scheduler (preemption and time slicing are played by the order of the calls, and by two threads
in case 7), `BasePriorityBooster` and the real `inInterrupt`, and the code that uses `PublishedPtr` (`Tools/kinematics-test` and
`Tools/macro-error-test` compile `Move::SetKinematics` and the stack functions of `GCodeBuffer` with it).

The Makefile reads the sources from this repository, `../..` (override with `RRF=`), and RRFLibraries from `../../../RRFLibraries`
(override with `RRFLIB=`). The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in
`.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # the plain build, AddressSanitizer + UBSan, and both without the checks, then run all four

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails, or, without the checks, if any case other
than the six that depend on them fails or one of those passes.
