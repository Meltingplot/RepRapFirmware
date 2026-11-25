# SBC code buffer allocation host test

This test runs RRF's real `SbcInterface::FillBuffer` from `src/SBC/SbcInterface.cpp` on a PC. The Makefile copies its text out of
the source at build time, as `Tools/laser-task-test` does. `sbc_code_buffer_alloc_test.cpp` gives it a minimal `SbcInterface` with
the code ring, a `GCodeBuffer`, `TaskCriticalSectionLocker` and `Tasks::AllocPermanent`.

## What is tested and why

In SBC mode the main task reads the codes that the SBC task has stored with `FillBuffer`. It walks the code ring inside a
`TaskCriticalSectionLocker`, which suspends the scheduler, and hands the next code of the channel to `GCodeBuffer::PutBinary`.
The buffer of a G-code buffer is allocated with `Tasks::AllocPermanent` (`GCodeBuffer::AllocateBinaryBuffer`) before its first code.
`AllocPermanent` takes the malloc mutex only while the scheduler is running
(`GetMallocMutex` in `src/Platform/Tasks.cpp`), so inside the locker it runs without it. A task of the same or a lower priority
that was preempted inside `malloc` could then be about to compare the heap top with the heap limit in `_sbrk`. In SBC mode no such
task exists (the network is not activated, `RepRap.cpp`), so this is latent, and it is fixed as every heap use inside a protected
region is. Up to 3.7-dev de1bfe598 the first code of every channel was put, and its buffer allocated by `BinaryParser::Put`
(`src/GCodes/GCodeBuffer/BinaryParser.cpp`), inside the locker. Now `FillBuffer` allocates it after the locker.

The test plays the locker with a counter and the G-code buffer with a stand-in whose `PutBinary` writes into the buffer, as
`BinaryParser::Put` does, and fails without one, and checks that no buffer is allocated with the scheduler suspended. It also checks
what `FillBuffer` does: the call that finds the first code of a channel without a buffer allocates the buffer after the locker and
leaves the code pending without moving the ring pointers, the next call reads it, a channel with a buffer reads its next code at
once, the code of another channel stays pending until its channel reads it and is read once, the ring pointers are reset when the
ring is read completely, and nothing is read or allocated while no code of the channel is pending.

## Running

```
make check
```
