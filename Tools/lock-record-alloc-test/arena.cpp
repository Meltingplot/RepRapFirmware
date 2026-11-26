// The heap that RRF's nano-mallocr.c grows with _sbrk, the real _sbrk and CoreAllocPermanent of CoreN2G, and the check of the free list.
// Compiled without AddressSanitizer, like nano-mallocr.c, because both touch the chunk headers and free-list links that the ASan build
// keeps poisoned.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "arena.h"
#include "HostRtos.h"

extern "C"
{
	alignas(16) char testArena[TestArenaSize];

	// CoreN2G src/syscalls.h:44 (heapTop = &_end) and :38 (heapLimit = _sstack), here the ends of the private arena
	char *testHeapTop = testArena;
	const char *testHeapLimit = testArena + TestArenaSize;
}

// CoreN2G src/CoreIO.h:353-372, which _sbrk and CoreAllocPermanent take since CoreN2G c9b23d7: IrqSave disables interrupts and
// IrqRestore restores PRIMASK. Played by the processor of host_rtos.cpp, which holds an interrupt that comes meanwhile until interrupts
// are enabled again, as the NVIC does with a pending interrupt when PRIMASK is cleared.
class AtomicCriticalSectionLocker
{
public:
	AtomicCriticalSectionLocker() noexcept { HostRtos::DisableInterrupts(); }
	~AtomicCriticalSectionLocker() noexcept { HostRtos::EnableInterrupts(); }
};

// The real _sbrk and CoreAllocPermanent of CoreN2G src/syscalls.h:50-86, which the Makefile extracts into gen/, on the private arena.
// nano-mallocr.c calls _sbrk through sbrk_aligned (nano-mallocr.c:264, :266, :277), and FreelistManager.h:51 calls CoreAllocPermanent
// for a lock record (through the recording CoreAllocPermanent of lock_record_alloc_test.cpp). The sbrk scenario's preemption point
// TestPreemptionPointSbrk is inserted before _sbrk's update of heapTop (syscalls.h:59); the interrupt it raises stays pending until the
// AtomicCriticalSectionLocker of _sbrk is released.
#define _ecv_array
#define heapTop					testHeapTop
#define heapLimit				testHeapLimit
#define _sbrk					test_sbrk
#define CoreAllocPermanent		test_CoreAllocPermanent
#define OutOfMemoryHandler		TestOutOfMemory
#define uint32_t				uintptr_t					// syscalls.h casts pointers to uint32_t, which is too narrow on the 64-bit host
#include "gen/syscalls-heap.inc"
#undef uint32_t

// The chunk layout of nano-mallocr.c:180-207
struct Chunk
{
	long size;
	Chunk *next;
};
extern "C" Chunk *test_malloc_free_list;								// nano-mallocr.c:249 (free_list, renamed by malloc_names.h)

// Walk the free list and check it against the live blocks. Returns the number of problems found.
unsigned int TestCheckFreeList(const TestBlock *live, unsigned int numLive, bool print) noexcept
{
	unsigned int problems = 0;
	const char *const top = testHeapTop;
	unsigned int n = 0;
	for (const Chunk *c = test_malloc_free_list; c != nullptr && n < 64; c = c->next, ++n)
	{
		const char *const start = reinterpret_cast<const char *>(c);
		const char *const end = start + c->size;
		if (print)
		{
			printf("        free chunk %u: [%p, %p) size %ld\n", n, (const void *)start, (const void *)end, c->size);
		}
		if (start < testArena || end > top || c->size < (long)(2 * sizeof(void *)))
		{
			printf("        FREE LIST BROKEN: chunk %p size %ld lies outside the heap [%p, %p)\n", (const void *)start, c->size, (const void *)testArena, (const void *)top);
			++problems;
			break;
		}
		if (c->next != nullptr && reinterpret_cast<const char *>(c->next) <= start)
		{
			printf("        FREE LIST BROKEN: chunk %p links back to %p (the list must be in address order, nano-mallocr.c:464-471)\n", (const void *)start, (const void *)c->next);
			++problems;
			break;
		}
		for (unsigned int i = 0; i < numLive; ++i)
		{
			const char *const b = static_cast<const char *>(live[i].ptr);
			if (b < end && start < b + live[i].size)
			{
				printf("        FREE LIST BROKEN: free chunk [%p, %p) overlaps the live block of %s [%p, %p)\n", (const void *)start, (const void *)end,
						live[i].what, (const void *)b, (const void *)(b + live[i].size));
				++problems;
			}
		}
	}
	if (print && n == 0)
	{
		printf("        free list empty\n");
	}
	return problems;
}

// End
