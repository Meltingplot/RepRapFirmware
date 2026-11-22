# M954 expansion mode host test

This test runs RRF's real `CanInterface::SwitchToExpansionMode` (M954) from `src/CAN/CanInterface.cpp`,
`GCodes::SwitchToExpansionMode` from `src/GCodes/GCodes3.cpp`, `FilamentMonitor::DeleteAll` from
`src/FilamentMonitors/FilamentMonitor.cpp` and `DeleteObject` from `src/RepRapFirmware.h` on a PC. The Makefile copies their text
out of the sources at build time, as `Tools/laser-task-test` does. `expansion_mode_alloc_test.cpp` gives them minimal stand-ins of
the classes and of the CAN state they use.

## What is tested and why

The switch to expansion mode deletes all filament monitors. `FilamentMonitor::DeleteAll` takes a `WriteLocker` on the monitor
list, and RRFLibraries' `ReadWriteLock::LockForWriting` allocates a lock record and may wait for a reader to finish; then it frees
each monitor with `delete`. RRF's `malloc` and `free` take the malloc mutex only while the scheduler is running (`GetMallocMutex` in
`src/Platform/Tasks.cpp`), and a task must not wait for a lock while the scheduler is suspended. Up to 3.7-dev de1bfe598,
`CanInterface::SwitchToExpansionMode` called `GCodes::SwitchToExpansionMode`, and so `DeleteAll`, inside its
`TaskCriticalSectionLocker`, which suspends the scheduler. M954 runs in the main task; the CAN receiver task switches when a main
board asks the board to enter test mode (`CommandProcessor.cpp`).

The test plays the locker with a counter and checks that the write lock is taken, and every monitor freed, with the scheduler
running. It also checks what the switch does: every filament monitor is deleted and its pointer cleared, the CAN address,
expansion mode and test mode are set, the axes are mapped to the local drivers, the move system is switched to expansion mode and
the CAN filters are reset once with the new address.

## Running

```
make check
```
