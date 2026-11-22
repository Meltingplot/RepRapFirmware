# Heap use inside protected regions check

This check fails when RepRapFirmware can reach the heap from a new place inside a protected region, unless `allowlist.txt` names
it. A protected region is code that runs
- with the scheduler suspended (`vTaskSuspendAll` to `xTaskResumeAll`),
- inside a FreeRTOS critical section (`vPortEnterCritical` to `vPortExitCritical`),
- with BASEPRI raised (`msr BASEPRI_MAX`),
- with interrupts disabled (`cpsid i`, `msr PRIMASK`),
- or in an exception handler.

The name `alloc-check` follows `pointer-check`: it is the check for allocation (and freeing) where the allocator is not safe.
The Meltingplot fork of Duet3Expansion has a copy of `alloc-check.py` in its `Tools/alloc-check`, which differs only in the
libraries that `LIBRARIES` names and in the name of the project in its texts. A fix to the script belongs in both copies.

## Why

RRF's heap is newlib's nano-malloc in `src/libc/nano-mallocr.c`. Its lock is `MALLOC_LOCK`, which is `GetMallocMutex`
(`nano-mallocr.c:134-135`). `GetMallocMutex` (`src/Platform/Tasks.cpp:113-120`) takes the malloc mutex only while the scheduler
is running. With the scheduler suspended, or before it starts, it takes nothing. `Tasks::AllocPermanent` (`Tasks.cpp:323-329`)
takes the same mutex around `CoreAllocPermanent`, which lowers the heap limit that `_sbrk` checks (CoreN2G `src/syscalls.h:49-78`).
This gives two failures:
- **The mutex is skipped.** Code that allocates or frees with the scheduler suspended runs `malloc` without the mutex. If a task
  of the same or a lower priority was preempted inside `malloc`, both work on the free list or the heap top at once and corrupt
  the heap. Preemption and time slicing are on (FreeRTOS `FreeRTOSConfig.h:77`, `:84`). `malloc` blocks only at the mutex,
  before it touches the heap, so a region in a task of priority p collides only with a task of priority p or lower that was
  preempted inside `malloc`. `Tools/lock-record-alloc-test` plays this with the real code for `ReadWriteLock` (finding K23).
- **The guard is lifted.** If the scheduler is running, taking the mutex can switch tasks, and a switch restores the BASEPRI
  and PRIMASK of the next task. A BASEPRI guard such as `BasePriorityBooster` is then gone while the code still relies on it.
  Upstream added `configASSERT(__get_BASEPRI() == 0 && __get_PRIMASK() == 0)` to `GetMallocMutex` for this (`Tasks.cpp:117`,
  cdcd49333). The assertion catches it at run time and resets the board; this check finds the path at build time.

In an exception handler the heap is never safe: the interrupted task may be inside `malloc`, and an ISR cannot take a mutex.

### History of the class

- RRF d4b10780d (D. Crocker, 2018-04-05, "More RTOS work again") introduced `GetMallocMutex` with the test "don't take mutex if
  scheduler not started or suspended" that is still there.
- RRFLibraries 2422ef6 (D. Crocker, 2024-10-08, "Fixed call to ::operator new inside a critical section"): the freelist
  `AllocateItem` (`General/FreelistManager.h`) called `::operator new` inside its own `TaskCriticalSectionLocker`. The fix leaves
  the locker before it allocates. On the same day D3E 0a1c2b65 ("Shortened an excessively long critical section") cut down a
  region in `ClosedLoop.cpp`.
- RRF cdcd49333 and D3E e847c690 (C. Hammacher, 2026-08-14, "Fixed step interrupt guard being lifted by memory allocation"):
  `Move::AddLinearSegments` could split a segment inside its `BasePriorityBooster`, and the split allocated. The fix calls
  `MoveSegment::PrimeFreeList` before the booster and adds the assertion to `GetMallocMutex`.
- This fork (2026-11-22): K23, `ReadWriteLock` allocating its lock records with the scheduler suspended (RRFLibraries #2), and
  the sites that `census.md` lists (RRF #72-#77).

The search covered the commit messages of RRF, RRFLibraries, CoreN2G, CANlib, the FreeRTOS fork and Duet3Expansion for
allocation with critical sections, suspension and interrupts. CoreN2G, CANlib and FreeRTOS have no fix of this class.

## What counts

**Regions.** The check knows the region primitives by name and by the instructions that their inline forms leave in the binary:
- `vTaskSuspendAll` and `xTaskResumeAll`. `RTOSIface::EnterTaskCriticalSection`/`LeaveTaskCriticalSection`,
  `TaskCriticalSectionLocker` and `ConditionalTaskCriticalSectionLocker` (RRFLibraries `src/RTOSIface/RTOSIface.h:350-412`) are
  inline and leave these calls.
- `vPortEnterCritical` and `vPortExitCritical`. `taskENTER_CRITICAL`, `EnterInterruptCriticalSection` and
  `InterruptCriticalSectionLocker` (FreeRTOS `portmacro.h:121-122`, `RTOSIface.h:323-386`) leave these calls.
- `msr BASEPRI_MAX` raises BASEPRI, `msr BASEPRI` restores it. This is what `ChangeBasePriority`, `RestoreBasePriority` and
  `BasePriorityBooster` leave (CoreN2G `src/CoreIO.h:379-412`), and FreeRTOS `ulPortRaiseBASEPRI` and `vPortSetBASEPRI`
  (`portmacro.h:228`, `:250`).
- `cpsid i` disables interrupts. `msr PRIMASK` takes the value it writes: a value read with `mrs` or loaded from memory restores
  the saved state, a constant sets it. This is what `IrqDisable`, `IrqSave`, `IrqRestore` (CoreN2G `src/Core.h:378-432`),
  `AtomicCriticalSectionLocker` (`CoreIO.h:353-372`) and `DisableInterrupts` (`RTOSIface.h:94`) leave.
- Exception handlers: every entry of `exception_table` (CoreN2G `Vectors.c`) after the initial stack pointer and the reset handler.

The check follows the control flow of each function: branches, conditional instructions in IT blocks, and the jump tables of
`tbb` and `tbh`, with the number of entries from the bounds check before them. It also knows two forms of Thumb-1 code, which
Duet3Expansion builds for the Cortex-M0+ of the SAMC21 and which a copy of this script checks there: a table of addresses that
`mov pc` jumps through, and a jump with `bl` to an address inside the same function. A function whose every exit leaves the
region state changed by the same amount (e.g. a function that only calls
`vTaskSuspendAll`) counts as a primitive for its callers. The FreeRTOS kernel (the functions compiled from `FreeRTOS/src`) is
not analysed as a region: it suspends the scheduler inline, and it does not allocate, because `configSUPPORT_DYNAMIC_ALLOCATION` is
0 (`FreeRTOSConfig.h:269`).

**Targets.** The heap functions of `nano-mallocr.c` (`malloc`, `free`, `realloc`, `calloc`, `cfree`, `memalign`, `valloc`,
`pvalloc`, `mallinfo`, `malloc_stats`, `_malloc_r`), `_sbrk` and `CoreAllocPermanent` (CoreN2G `syscalls.h`),
`GetMallocMutex`/`ReleaseMallocMutex`, and newlib's `__malloc_lock`/`__malloc_unlock`, also as clones that the compiler splits off
(`GetMallocMutex.part.0`). The allocation functions count as targets themselves: `operator new` and `delete`,
`Tasks::AllocPermanent`, `MessageBufferAlloc` (through which CANlib's `CanMessageBuffer` allocates, CANlib
`src/CanMessageBuffer.h`) and the exception allocation of a `throw` (`__cxa_allocate_exception`, `src/libcpp/eh_alloc.cc`), so that
a call to one reaches the heap even when the compiler has inlined the heap functions into it. Everything that calls a target
directly or through other direct calls reaches the heap, for example the freelist allocators, which call `operator new` when their
list is empty. The key names the first allocation function on the chain, or the heap function itself.

**Calls through a register.** A call through a function pointer, a virtual call or a `function_ref` has a target that the check
cannot see. Each one inside a region, and each direct call inside a region to a function that can reach one, gives a finding with
the target `indirect call`. The calls through a register that a literal load resolves (`ldr rN, =function` then `blx rN`) and the
`__*_veneer` stubs are followed. The reach towards calls through a register stops at functions that never return
(`vAssertCalled`, `SoftwareReset`, ...): they reset the board, or they throw, and the heap chains find a throw.

## Key format

    Opener [kind] -> target

`Opener` is the function in whose code the region is open, with the class and namespace but without the parameter list. A static
function or one in an anonymous namespace gets its file: `src/Foo.cpp: Opener [kind] -> target`. `kind` is the first of
`suspended`, `critical`, `basepri` or `primask` that is in effect at the call, or `isr` for an exception handler. `target` is as
above. A call that reaches both the heap and a call through a register gives only the heap key.

Each new finding gives one line, in the format that compilers use, with the outermost source line of the call:

    src/Platform/Event.cpp:58: error: heap reached inside a protected region: Event::AddEventV [suspended] -> operator new: ...

The message has up to three call sites with their inlined functions and the chain to the heap. The exit code is 1 if there is a
new finding. `--verbose` also prints the allowlisted findings and the notes: jump tables without a bounds check, `msr` values the
check does not know, and functions that change the region state on some paths only. `--query FUNCTION` prints the chains from a
function to the heap and to calls through a register.

## What the check does not see

- **Targets of calls through a register.** They are findings of their own; `census.md` names the targets that the firmware sets
  and why they do not reach the heap. A new target of an existing call (a new override of a virtual function, a new callback in an
  interrupt table) does not make a new finding. The census of the entry has to be read again then.
- **Correlated conditions.** The analysis does not track values. A call on a path that cannot run inside the region (because of
  a test the check does not evaluate) is still found. `SbcInterface::FillBuffer` is such a case after its fix.
- **The run-time context of a function.** A function that is only ever called with the scheduler suspended, but that does not
  suspend it itself, is a region only through its callers' calls. A region that a caller opens and a callee closes (or the other
  way round) on some paths only is taken as unchanged, with a note.
- **Values written by `msr`** that are neither read with `mrs`, loaded from memory nor a constant: taken as a restore
  (`BASEPRI_MAX`: as a raise), with a note.
- **Configurations that it does not build.** The default is those of `make all` (`Duet3_MB6HC`, `Duet3_MB6XD`,
  `Duet3Mini5plus`), the same as `Tools/pointer-check`.
- **Allocation that is inlined under another name.** A function into which the compiler has inlined an allocation function
  together with the mutex functions calls no target. `MessageBufferAlloc` is a target for this reason: in the EXP1HCL build of
  Duet3Expansion it contains all of `Tasks::AllocPermanent`.
- **Other errors in regions.** A blocking call (a mutex, a `ReadWriteLock`, a notification wait) inside a region is a different
  error and only found where it also reaches the heap.

## Use

The Makefile needs the Arm GNU toolchain (`CROSS_COMPILE`, default `arm-none-eabi-`: `objdump`, `nm`, `addr2line`) and python3.
The firmware build container of DuetSoftwareFramework has both. The ELF files are those that `Tools/pointer-check` builds with
`-g`; the check reads the source lines from their debug information.

    make check                      # build the configurations with -g (Tools/pointer-check), then check them
    make check CONFIGS=Duet3_MB6HC  # only this configuration; entries of the others show up as notes
    make scan                       # check the ELF files that are already there, without building
    make update                     # rewrite allowlist.txt from the ELF files that are there
    make selftest                   # check alloc-check.py against selftest/src/sample.cpp and no-alloc.cpp

One run takes a few seconds per ELF file. `selftest/src/sample.cpp` has positive and negative cases: inline and out-of-line
regions, a callee that allocates, `cpsid i`, `BASEPRI_MAX`, a call through a function pointer, a static function, a jump table,
an exception handler. `selftest/expected.txt` has their keys. `no-alloc.cpp` has none: `make selftest` runs `--update` on it, which
must write an allowlist with the header and no entries.

## A new finding

Allocate before the region and free after it, as `MoveSegment::PrimeFreeList` and the fixes in `census.md` do. If that is not
possible, add the key from the error message to `allowlist.txt` with the reason why the heap is not reached there. When a finding
goes away, the check prints a note, and `make update` removes it from the list.

The Meltingplot release workflow (`.github/workflows/mp-release.yml`) runs `make selftest` and `make scan` right after the raw
pointer check, on the ELF files it has built. A new finding stops the release.

The firmware builds do not use this folder: make and CMake only read `src/`.
