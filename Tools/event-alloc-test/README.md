# Event allocation host test

This test runs RRF's real `Event::AddEvent`, `Event::AddEventV`, `Event::FinishedProcessing` and the `Event` constructor from
`src/Platform/Event.cpp` on a PC. The Makefile copies their text and the definitions of the static members out of the source at
build time, as `Tools/laser-task-test` does, because `Event.cpp` as a whole needs the object model and the hardware headers.
`event_alloc_test.cpp` gives them a minimal `Event` with the members of `src/Platform/Event.h`, `TaskCriticalSectionLocker` and
the text buffer.

## What is tested and why

`Event` uses the freelist `new` of RRFLibraries (`DECLARE_FREELIST_NEW_DELETE` in `Event.h`, `General/FreelistManager.h`). When
the freelist is empty it calls `::operator new`, and so RRF's `malloc`. That takes the malloc mutex only while the scheduler is
running (`GetMallocMutex` in `src/Platform/Tasks.cpp`), so a `malloc` with the scheduler suspended runs without it, and a task
that was preempted inside `malloc` and this one then work on the heap at once. Up to 3.7-dev de1bfe598, `AddEventV` allocated the
new event inside its `TaskCriticalSectionLocker`, which suspends the scheduler. It runs in the tasks that raise events: the Heat
task (heater faults), the main task (driver events, M957, filament errors) and the CAN receiver task (events from expansion
boards).

The test plays the locker with a counter and the freelist with an empty one, so that every new event comes from the heap, and
checks that no event is allocated with the scheduler suspended. It also checks what `AddEventV` does: events are queued in
priority order and the same type in arrival order, a similar event (same type, device number, parameter and CAN address) is not
queued and its allocation is released, an event from another CAN board is not similar, the event that is being processed stays
at the head of the queue and suppresses similar ones, and `FinishedProcessing` releases the processed event.

The freelist `delete` puts the event on the freelist and never frees memory (`ReleaseItem` in `FreelistManager.h`), so the test
does not require it to run with the scheduler running; `AddEventV` releases an unused event after the locker all the same.

## Running

```
make check
```
