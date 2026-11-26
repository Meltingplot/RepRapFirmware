// Host test of the lock records of ReadWriteLock (RRFLibraries src/RTOSIface/RTOSIface.cpp). ReadWriteLock allocates a lock record with
// the scheduler suspended. Up to RRFLibraries 696a015 that ran RRF's malloc without the malloc mutex, because GetMallocMutex took the
// mutex only while the scheduler was running; if a lower-priority task was preempted inside malloc, both worked on the heap at once.
// Since RRFLibraries d8708f0 the free list takes a new record from CoreAllocPermanent, which is allowed with the scheduler suspended,
// and since RRF d85662412 GetMallocMutex takes the mutex with the scheduler suspended too (src/Platform/Tasks.cpp:113-120), so that
// FreeRTOS asserts (queue.c:1682) instead of malloc running unprotected.
//
// Real code under test (see the Makefile for where each part comes from):
//  - the ReadWriteLock and Mutex members (RRFLibraries src/RTOSIface/RTOSIface.cpp), RTOSIface.h, General/FreelistManager.h (the
//    LockRecord free list and where it takes a new record from), from the RRFLibraries tree that the Makefile's RRFLIB names
//  - GetMallocMutex / ReleaseMallocMutex (RRF src/Platform/Tasks.cpp:108-128)
//  - malloc and free (RRF src/libc/nano-mallocr.c) on a private arena, with _sbrk and CoreAllocPermanent of CoreN2G src/syscalls.h
//  - vTaskSuspendAll and xTaskGetSchedulerState (FreeRTOS src/tasks.c)
// Stand-ins: the single-core processor and scheduler of host_rtos.cpp (with the interrupt mask of AtomicCriticalSectionLocker), and
// ::operator new / delete below (libsupc++ new_op.cc calls malloc).
//
// Two tasks, with the priorities that RRF gives them (src/Platform/TaskPriorities.h):
//  MAIN (TaskPriority::SpinPriority = 1) calls malloc. HEAT (TaskPriority::HeatPriority = 3) waits for its next sample time, as
//  Heat::HeaterTask does (Heat.cpp:319), then takes a read lock on the sensor list (Heat.cpp:365) and polls a sensor, which may wait for the
//  shared SPI bus (SpiTemperatureSensor.cpp:51 -> SharedSpiClient.cpp:29) while it holds the lock.
//
// Usage: lock-record-alloc-test <split|sbrk> <race|freelist|outside>
//        lock-record-alloc-test calls
//  split     the interrupt comes while MAIN's malloc splits the first free chunk (before nano-mallocr.c:326; only the hook326 builds have it)
//  sbrk      the interrupt comes while MAIN's malloc grows the heap (inside _sbrk, before its update of heapTop; since CoreN2G c9b23d7
//            interrupts are disabled there, so it is taken when _sbrk enables them again, still inside malloc)
//  race      the interrupt that wakes HEAT comes at that point inside MAIN's malloc
//  freelist  the same, but a lock record is already on the free list (a lock was taken and released before)
//  outside   the interrupt comes just after MAIN's malloc has returned
//  calls     MAIN alone calls each of the four functions that take a lock record (LockForReading, ConditionalLockForReading,
//            LockForWriting, ConditionalLockForWriting) when the free list is empty, and each path that needs no record
// The check of every run: the live blocks (malloc's and the lock records) do not overlap, malloc's free list is intact, and
// ::operator new is never called with the scheduler suspended.
// Exit code: 0 no corruption found and no ::operator new with the scheduler suspended, 1 corruption found, 3 vAssertCalled (the board
// would reset), 5 ::operator new was called with the scheduler suspended, ASan's own code when it reports.

#include <RTOSIface/RTOSIface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>

#include "arena.h"

#if defined(__SANITIZE_ADDRESS__)
# include <sanitizer/asan_interface.h>
# define TEST_POISON(_p, _n)		ASAN_POISON_MEMORY_REGION((_p), (_n))
# define TEST_UNPOISON(_p, _n)	ASAN_UNPOISON_MEMORY_REGION((_p), (_n))
#else
# define TEST_POISON(_p, _n)		((void)(_p), (void)(_n))
# define TEST_UNPOISON(_p, _n)	((void)(_p), (void)(_n))
#endif

void TestCreateMallocMutex() noexcept;								// malloc_mutex.cpp
TaskHandle TestMallocMutexHolder() noexcept;

constexpr uint32_t HeatNotifyIndex = 1;							// any index other than NotifyIndices::ReadWriteLocker (0)
constexpr unsigned int MainPriority = 1;							// TaskPriority::SpinPriority, TaskPriorities.h:15
constexpr unsigned int HeatPriority = 3;							// TaskPriority::HeatPriority, TaskPriorities.h:19

enum class Point { none, split, sbrk };
enum class Variant { race, freelist, outside, calls };

static Point scenario = Point::none;
static Variant variant = Variant::race;
static Point armed = Point::none;
static bool preempted = false;
static unsigned int newsWhileSuspended = 0;							// calls of ::operator new with the scheduler suspended

static TaskBase mainTask;
static TaskBase heatTask;
static ReadWriteLock sensorsLock;									// stands for Heat::sensorsLock (Heat.cpp:154)

// Live blocks of the arena
static TestBlock live[32];
static unsigned int numLive = 0;
static void *heatRecord = nullptr;									// the new lock record that HEAT got, if any
static size_t heatRecordSize = 0;

static void AddLive(void *p, size_t n, const char *what) noexcept
{
	if (numLive < sizeof(live)/sizeof(live[0]))
	{
		live[numLive++] = TestBlock{ p, n, what };
	}
}

static size_t RemoveLive(void *p) noexcept
{
	for (unsigned int i = 0; i < numLive; ++i)
	{
		if (live[i].ptr == p)
		{
			const size_t n = live[i].size;
			live[i] = live[--numLive];
			return n;
		}
	}
	return 0;
}

static const char *SchedulerStateText() noexcept
{
	switch (xTaskGetSchedulerState())
	{
	case taskSCHEDULER_RUNNING:		return "RUNNING";
	case taskSCHEDULER_SUSPENDED:	return "SUSPENDED";
	default:						return "NOT_STARTED";
	}
}

// libsupc++ new_op.cc: operator new calls malloc, which on the board is RRF's nano-mallocr.c (in one MB6HC 3.7-dev build _Znwj at
// 0x4ccdc0 calls malloc at 0x400568; the addresses change with every build). In this test only the LockRecord free list of an
// RRFLibraries before d8708f0 calls it, when the list is empty (FreelistManager.h:47 in 696a015).
void *operator new(std::size_t sz)
{
	StaticTask_t *const t = HostRtos::Current();
	if (HostRtos::Suspended() != 0)
	{
		++newsWhileSuspended;
	}
	if (t != nullptr)
	{
		const TaskHandle holder = TestMallocMutexHolder();
		HostRtos::Log("%s: ::operator new(%zu) for a LockRecord; scheduler %s, uxSchedulerSuspended %u, mallocMutex held by %s",
						HostRtos::Name(t), sz, SchedulerStateText(), HostRtos::Suspended(), HostRtos::Name(holder));
	}
	void *const p = test_malloc((sz == 0) ? 1 : sz);
	if (p == nullptr)
	{
		TestOutOfMemory();
	}
	TEST_UNPOISON(p, sz);
	AddLive(p, sz, (t == &heatTask) ? "HEAT's LockRecord" : "a LockRecord");
	if (t == &heatTask)
	{
		heatRecord = p;
		heatRecordSize = sz;
	}
	if (t != nullptr)
	{
		HostRtos::Log("%s: ::operator new returned %p", HostRtos::Name(t), p);
	}
	return p;
}

void operator delete(void *p) noexcept
{
	if (p != nullptr)
	{
		TEST_POISON(p, RemoveLive(p));
		test_free(p);
	}
}

void operator delete(void *p, std::size_t) noexcept
{
	operator delete(p);
}

// FreelistManager.h:51 takes a new item from CoreAllocPermanent when its free list is empty (RRFLibraries d8708f0). This records the
// block and calls the real CoreAllocPermanent of CoreN2G src/syscalls.h:74, compiled in arena.cpp.
void *CoreAllocPermanent(size_t sz, std::align_val_t align) noexcept
{
	StaticTask_t *const t = HostRtos::Current();
	HostRtos::Log("%s: CoreAllocPermanent(%zu) for a LockRecord (FreelistManager.h:51); scheduler %s, uxSchedulerSuspended %u, mallocMutex held by %s",
					HostRtos::Name(t), sz, SchedulerStateText(), HostRtos::Suspended(), HostRtos::Name(TestMallocMutexHolder()));
	void *const p = test_CoreAllocPermanent(sz, align);
	TEST_UNPOISON(p, sz);
	AddLive(p, sz, (t == &heatTask) ? "HEAT's LockRecord" : "a LockRecord");
	if (t == &heatTask)
	{
		heatRecord = p;
		heatRecordSize = sz;
	}
	HostRtos::Log("%s: CoreAllocPermanent returned %p", HostRtos::Name(t), p);
	return p;
}

static void *TestMalloc(size_t n, const char *what) noexcept
{
	void *const p = test_malloc(n);
	TEST_UNPOISON(p, n);
	AddLive(p, n, what);
	return p;
}

static void TestFree(void *p) noexcept
{
	TEST_POISON(p, RemoveLive(p));
	test_free(p);
}

// The interrupt that wakes HEAT, at a point inside MAIN's malloc
static void Preempt(Point here, const char *where) noexcept
{
	if (armed == here && HostRtos::Current() == &mainTask)
	{
		armed = Point::none;
		preempted = true;
		HostRtos::Log("MAIN is inside malloc at %s, mallocMutex held by %s, scheduler %s", where, HostRtos::Name(TestMallocMutexHolder()), SchedulerStateText());
		HostRtos::Interrupt(&heatTask);								// the tick ends HEAT's wait for its sample time
	}
}

// The target addresses are those of one MB6HC 3.7-dev build and change with every build
extern "C" void TestPreemptionPointSplit()
{
	Preempt(Point::split, "nano-mallocr.c:326 (splitting the first free chunk; target: between ldr free_list and str free_list, 0x400588 and 0x400622 in one MB6HC build)");
}

extern "C" void TestPreemptionPointSbrk()
{
	Preempt(Point::sbrk, "_sbrk (before its update of heapTop, CoreN2G syscalls.h:59), called from nano-mallocr.c:266 via :361");
}

extern "C" [[noreturn]] void TestOutOfMemory()
{
	printf("RESULT: out of memory in the test arena\n");
	fflush(stdout);
	_exit(2);
}

// On the board vAssertCalled resets the processor (RTOSIface.cpp:19-20, FreeRTOSConfig.h:437, nano-mallocr.c:71)
extern "C" [[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept
{
	printf("RESULT: vAssertCalled(%u, %s): the board would reset here\n", (unsigned int)line, file);
	fflush(stdout);
	_exit(3);
}

static unsigned int overlaps = 0;

static bool Overlap(const void *a, size_t na, const void *b, size_t nb) noexcept
{
	const char *const ca = static_cast<const char *>(a);
	const char *const cb = static_cast<const char *>(b);
	return ca < cb + nb && cb < ca + na;
}

static void Check(const char *when) noexcept
{
	printf("    check %s:\n", when);
	for (unsigned int i = 0; i < numLive; ++i)
	{
		printf("        live block %u: [%p, %p) %s\n", i, live[i].ptr, (void *)((char *)live[i].ptr + live[i].size), live[i].what);
	}
	for (unsigned int i = 0; i < numLive; ++i)
	{
		for (unsigned int j = i + 1; j < numLive; ++j)
		{
			if (Overlap(live[i].ptr, live[i].size, live[j].ptr, live[j].size))
			{
				printf("        HEAP CORRUPT: the live blocks of %s and %s overlap\n", live[i].what, live[j].what);
				++overlaps;
			}
		}
	}
	overlaps += TestCheckFreeList(live, numLive, true);
	fflush(stdout);
}

// The "calls" variant: MAIN takes one lock with each function that needs a new lock record and keeps it, so that the free list stays
// empty and every lock takes a new record. Then it runs the paths that need no record, while the free list is still empty.
static ReadWriteLock locks[4];

static void CallsTaskCode(void *) noexcept
{
	HostRtos::Log("MAIN: LockForReading, LockForWriting, ConditionalLockForReading and ConditionalLockForWriting on four free locks");
	locks[0].LockForReading();
	locks[1].LockForWriting();
	const bool gotRead = locks[2].ConditionalLockForReading();
	const bool gotWrite = locks[3].ConditionalLockForWriting();
	HostRtos::Log("MAIN: the paths that need no record: a second read lock, a read lock and a second write lock by the write lock owner,"
					" and a conditional write lock that fails because of the read lock");
	locks[0].LockForReading();
	locks[1].LockForReading();
	locks[1].LockForWriting();
	const bool gotRead1 = locks[1].ConditionalLockForReading();
	const bool gotWrite0 = locks[0].ConditionalLockForWriting();
	if (!gotRead || !gotWrite || !gotRead1 || gotWrite0)
	{
		printf("RESULT: a conditional lock returned the wrong result (%d %d %d %d, expected 1 1 1 0)\n", gotRead, gotWrite, gotRead1, gotWrite0);
		fflush(stdout);
		_exit(6);
	}
	locks[0].ReleaseReader();
	locks[0].ReleaseReader();
	locks[1].ReleaseReader();
	locks[1].ReleaseReader();
	locks[1].ReleaseWriter();
	locks[1].ReleaseWriter();
	locks[2].ReleaseReader();
	locks[3].ReleaseWriter();
	HostRtos::Log("MAIN: done");
}

static void MainTaskCode(void *) noexcept
{
	HostRtos::Log("MAIN runs");
	if (variant == Variant::freelist)
	{
		HostRtos::Log("MAIN: takes and releases a read lock first, so a lock record is on the free list (FreelistManager.h:50-56)");
		ReadLocker lock(sensorsLock);
	}
	void *const keep = TestMalloc(40, "MAIN's earlier object");		// the heap is in use before the race
	(void)keep;
	if (scenario == Point::split)
	{
		void *const p0 = TestMalloc(64, "MAIN's freed object");
		TestFree(p0);												// leaves one free chunk at the head of the free list
	}
	Check("before MAIN's malloc");

	armed = (variant == Variant::outside) ? Point::none : scenario;
	HostRtos::Log("MAIN: malloc(24)");
	void *const mine = TestMalloc(24, "MAIN's new object");
	HostRtos::Log("MAIN: malloc(24) returned %p", mine);
	if (variant == Variant::outside)
	{
		HostRtos::Log("MAIN has left malloc");
		HostRtos::Interrupt(&heatTask);								// the tick comes after malloc has returned
	}
	if (!preempted && variant != Variant::outside)
	{
		printf("RESULT: the preemption point was not reached (this build has no hook for it)\n");
		fflush(stdout);
		_exit(4);
	}
	Check("after MAIN's malloc returned");

	HostRtos::Log("MAIN: initialises its object (memset 0xA5), uses it and frees it");
	memset(mine, 0xA5, 24);
	TestFree(mine);

	HostRtos::Log("MAIN: the SPI transfer that HEAT waits for completes");
	HostRtos::Interrupt(&heatTask);
	HostRtos::Log("MAIN: done");
}

static void HeatTaskCode(void *) noexcept
{
	HostRtos::Log("HEAT runs and waits for its next sample time (Heat.cpp:319)");
	TaskBase::TakeIndexed(HeatNotifyIndex);
	HostRtos::Log("HEAT: takes the read lock on the sensor list (Heat.cpp:365)");
	{
		ReadLocker lock(sensorsLock);
		if (heatRecord != nullptr)
		{
			HostRtos::Log("HEAT: holds the read lock, its lock record is the new block %p", heatRecord);
		}
		else
		{
			HostRtos::Log("HEAT: holds the read lock, its lock record came from the lock record free list");
		}
		HostRtos::Log("HEAT: polls a sensor and waits for the shared SPI bus (SpiTemperatureSensor.cpp:51, SharedSpiClient.cpp:29)");
		TaskBase::TakeIndexed(HeatNotifyIndex);
		HostRtos::Log("HEAT: releases the read lock (ReleaseReader, RTOSIface.cpp)");
	}
	HostRtos::Log("HEAT: released the read lock");
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool calls = (argc == 2 && strcmp(argv[1], "calls") == 0);
	if (calls)
	{
		variant = Variant::calls;
	}
	else if (argc == 3)
	{
		scenario = (strcmp(argv[1], "split") == 0) ? Point::split : (strcmp(argv[1], "sbrk") == 0) ? Point::sbrk : Point::none;
		variant = (strcmp(argv[2], "freelist") == 0) ? Variant::freelist : (strcmp(argv[2], "outside") == 0) ? Variant::outside : Variant::race;
	}
	if (!calls && (argc != 3 || scenario == Point::none || (strcmp(argv[2], "race") != 0 && variant == Variant::race)))
	{
		fprintf(stderr, "usage: %s <split|sbrk> <race|freelist|outside>\n       %s calls\n", argv[0], argv[0]);
		return 64;
	}
	printf("%s %s %s, RRFLibraries %s\n", argv[0], argv[1], (calls) ? "" : argv[2], TEST_RRFLIB);
	TEST_POISON(testArena, TestArenaSize);
	TestCreateMallocMutex();
	if (calls)
	{
		HostRtos::CreateTask(&mainTask, "MAIN", MainPriority, CallsTaskCode, nullptr);
	}
	else
	{
		HostRtos::CreateTask(&mainTask, "MAIN", MainPriority, MainTaskCode, nullptr);
		HostRtos::CreateTask(&heatTask, "HEAT", HeatPriority, HeatTaskCode, nullptr);
	}
	HostRtos::StartScheduler();
	Check("at the end");
	if (overlaps != 0)
	{
		printf("RESULT: HEAP CORRUPT (%u problems)\n", overlaps);
		fflush(stdout);
		_exit(1);
	}
	if (newsWhileSuspended != 0)
	{
		printf("RESULT: ::operator new was called %u times with the scheduler suspended\n", newsWhileSuspended);
		fflush(stdout);
		_exit(5);
	}
	printf("RESULT: no corruption found, no ::operator new with the scheduler suspended\n");
	fflush(stdout);
	_exit(0);
}

// End
