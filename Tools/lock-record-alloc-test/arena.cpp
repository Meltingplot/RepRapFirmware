// The heap that RRF's nano-mallocr.c grows with _sbrk, and the check of its free list. Compiled without AddressSanitizer, like
// nano-mallocr.c, because both touch the chunk headers and free-list links that the ASan build keeps poisoned.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "arena.h"

extern "C"
{
	alignas(16) char testArena[TestArenaSize];

	// CoreN2G src/syscalls.h:44 (heapTop = &_end) and :38 (heapLimit = _sstack), here the ends of the private arena
	char *testHeapTop = testArena;
	const char *testHeapLimit = testArena + TestArenaSize;

	// Stand-in for CoreN2G src/syscalls.h:49-64 (_sbrk), which nano-mallocr.c calls through sbrk_aligned (nano-mallocr.c:264, :266, :277).
	// The compiled function loads heapTop once (ldr r0, [r2]), adds incr, compares with heapLimit and stores the new top (str r3, [r2]),
	// then returns the value it loaded. The addresses here and below are those of one MB6HC 3.7-dev build (Duet3Firmware_MB6HC.elf,
	// _sbrk at 0x48daf8) and change with every build.
	// This stand-in has the same order of the load and the store; TestPreemptionPointSbrk stands for an interrupt between them, which the
	// target allows because _sbrk runs with interrupts enabled (Tasks.cpp:117 asserts that BASEPRI and PRIMASK are 0 in GetMallocMutex).
	void *test_sbrk(ptrdiff_t incr)
	{
		char *const prevHeap = testHeapTop;							// ldr r0, [r2]		0x48db00
		if (incr != 0)
		{
			TestPreemptionPointSbrk();
		}
		char *const newHeap = prevHeap + incr;						// add r3, r0		0x48db04
		if (newHeap <= testHeapLimit)								// cmp r1, r3		0x48db06
		{
			testHeapTop = newHeap;									// str r3, [r2]		0x48db0a
			return prevHeap;
		}
		TestOutOfMemory();											// OutOfMemoryHandler, syscalls.h:59
		return reinterpret_cast<void *>(-1);
	}
}

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
