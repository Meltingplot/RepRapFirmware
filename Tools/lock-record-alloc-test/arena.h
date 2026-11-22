// The interface between the test, the private heap arena and RRF's nano-mallocr.c (renamed by malloc_names.h)

#ifndef TEST_ARENA_H
#define TEST_ARENA_H

#include <cstddef>

constexpr size_t TestArenaSize = 16384;

struct TestBlock
{
	void *ptr;
	size_t size;
	const char *what;
};

extern "C"
{
	extern char testArena[TestArenaSize];
	extern char *testHeapTop;
	extern const char *testHeapLimit;

	void *test_malloc(size_t s);										// RRF src/libc/nano-mallocr.c:289 (nano_malloc = malloc)
	void test_free(void *p);											// nano-mallocr.c:473

	void *test_sbrk(ptrdiff_t incr);
	void TestPreemptionPointSbrk();									// lock_record_alloc_test.cpp: an interrupt inside _sbrk
	void TestPreemptionPointSplit();									// lock_record_alloc_test.cpp: an interrupt before nano-mallocr.c:326
	[[noreturn]] void TestOutOfMemory();
}

unsigned int TestCheckFreeList(const TestBlock *live, unsigned int numLive, bool print) noexcept;

#endif
