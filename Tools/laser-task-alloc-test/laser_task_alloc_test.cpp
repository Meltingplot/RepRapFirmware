// Host test of the allocation of the laser task by Move::CreateLaserTask (src/Movement/Move.cpp), compiled from the real text of the
// function and of the definitions of Move::laserTask and Move::laserTaskLock that the Makefile extracts.
//
// Task<> has no operator new of its own (RRFLibraries src/RTOSIface/RTOSIface.h), so the laser task comes from ::operator new and
// so from RRF's malloc, which must not run with the scheduler suspended (GetMallocMutex in src/Platform/Tasks.cpp). CreateLaserTask
// is called when laser mode or IOBits is selected (M452, M670), a scanning Z probe is configured (EndstopsManager.cpp), M571 is used
// (Platform.cpp) or extruder heater feedforward is configured (Tool.cpp). Up to 3.7-dev de1bfe598 it allocated the task inside its
// TaskCriticalSectionLocker, which suspends the scheduler. Upstream 5bc928ebf allocated it before the locker, and c37cac98f replaced
// the locker by a write lock on laserTaskLock around the check, the allocation and the creation, and publishes the task pointer only
// after Create, so that two callers cannot both create the task and nothing notifies a task that has not been created yet.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter, ReadWriteLock and WriteLocker with a flag and a
// waiting task, Task<> with a stand-in whose Create records the call, and ::operator new and delete with the host heap. No
// allocation or free may happen with the scheduler suspended, and the task pointer must not be set before Create. It also checks what
// CreateLaserTask does: it creates the task once, a second call does nothing, and another task that calls it while this one allocates
// the task (played inside operator new) waits for the write lock and creates no second task.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#define _ecv_null

namespace TaskPriority
{
	constexpr unsigned int LaserPriority = 5;					// src/Platform/TaskPriorities.h
}

static unsigned int suspended = 0;							// uxSchedulerSuspended
static bool checking = false;									// true while a case runs CreateLaserTask
static std::vector<std::string> failures;
static unsigned int taskAllocations = 0, taskFrees = 0, creates = 0;
static void (*duringAllocation)() = nullptr;					// another task that runs while CreateLaserTask allocates, if it can

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

// Just what CreateLaserTask uses of ReadWriteLock and WriteLocker (RRFLibraries RTOSIface.h). LockForWriting suspends the scheduler
// only while it updates its lock records, so the holder runs, and allocates, with the scheduler running. Played on one thread: a task
// that asks for the write lock while another one holds it waits until that one releases it (LockForWriting in RTOSIface.cpp), so the
// waiting task runs when the lock is released.
class ReadWriteLock
{
public:
	bool writeLocked = false;
	void (*waiter)() = nullptr;										// the task that waits for the write lock, if any
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock& p_lock) noexcept : lock(p_lock) { lock.writeLocked = true; }
	~WriteLocker() noexcept
	{
		lock.writeLocked = false;
		if (lock.waiter != nullptr)
		{
			void (*const waiting)() = lock.waiter;
			lock.waiter = nullptr;
			waiting();
		}
	}

private:
	ReadWriteLock& lock;
};

typedef void (*TaskFunction_t)(void *);
class TaskBase;
static bool IsPublished(const TaskBase *t) noexcept;

// Just what CreateLaserTask uses of TaskBase and Task (RRFLibraries RTOSIface.h). As there, a task that was never created has a zero
// taskId, and the destructor of such a task does nothing (TaskBase::TerminateAndUnlink in RTOSIface.cpp).
class TaskBase
{
public:
	TaskBase() noexcept : taskId(0) { }
	~TaskBase() noexcept { }

	uint32_t taskId;
};

template<unsigned int StackWords> class Task : public TaskBase
{
public:
	void Create(TaskFunction_t pxTaskCode, const char *pcName, void *_ecv_null pvParameters, unsigned int uxPriority) noexcept
	{
		(void)pxTaskCode; (void)pcName; (void)pvParameters; (void)uxPriority;
		if (taskId != 0)
		{
			failures.push_back("Create was called twice on the same task");
		}
		if (IsPublished(this))
		{
			failures.push_back("the task pointer was set before the task was created");
		}
		taskId = 1;
		++creates;
	}

private:
	uint32_t stack[StackWords];
};

extern "C" void LaserTaskStart(void *) noexcept { }

// The members of Move in src/Movement/Move.h that CreateLaserTask uses
class Move
{
public:
	static void CreateLaserTask() noexcept;

	static constexpr size_t LaserTaskStackWords = 300;
	static Task<LaserTaskStackWords> *_ecv_null laserTask;
	static ReadWriteLock laserTaskLock;
};

#include "CreateLaserTask.inc"

static bool IsPublished(const TaskBase *t) noexcept
{
	return Move::laserTask == t;
}

// The host heap, with the check. Only the allocations of a laser task are counted, by their size.
void *operator new(size_t sz)
{
	const bool isTask = checking && sz == sizeof(Task<Move::LaserTaskStackWords>);
	if (isTask)
	{
		if (suspended != 0)
		{
			failures.push_back("the laser task was allocated with the scheduler suspended");
		}
		++taskAllocations;
	}
	void *const p = malloc(sz);
	if (isTask && duringAllocation != nullptr && suspended == 0)		// another task can run only while the scheduler is running
	{
		void (*const other)() = duringAllocation;
		duringAllocation = nullptr;
		if (Move::laserTaskLock.writeLocked)
		{
			Move::laserTaskLock.waiter = other;						// it waits for the write lock
		}
		else
		{
			other();
		}
	}
	return p;
}

void operator delete(void *p) noexcept
{
	free(p);
}

void operator delete(void *p, size_t sz) noexcept
{
	if (checking && p != nullptr && sz == sizeof(Task<Move::LaserTaskStackWords>))
	{
		if (suspended != 0)
		{
			failures.push_back("a laser task was freed with the scheduler suspended");
		}
		++taskFrees;
	}
	free(p);
}

static unsigned int passed = 0, failed = 0;

static void Case(const char *name, unsigned int calls, void (*other)(), unsigned int expectedAllocations, unsigned int expectedFrees) noexcept
{
	failures.clear();
	taskAllocations = taskFrees = creates = 0;
	duringAllocation = other;
	checking = true;
	for (unsigned int i = 0; i < calls; ++i)
	{
		Move::CreateLaserTask();
	}
	checking = false;

	std::string why;
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	if (suspended != 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("the scheduler was left suspended");
	}
	if (creates != 1 || Move::laserTask == nullptr || Move::laserTask->taskId == 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("the laser task was created ") + std::to_string(creates) + " times";
	}
	if (taskAllocations != expectedAllocations || taskFrees != expectedFrees)
	{
		why += (why.empty() ? "" : "; ") + std::string("allocations ") + std::to_string(taskAllocations) + ", frees " + std::to_string(taskFrees)
				+ ", expected " + std::to_string(expectedAllocations) + " and " + std::to_string(expectedFrees);
	}
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s: %s\n", name, why.c_str());
	}
	delete Move::laserTask;
	Move::laserTask = nullptr;
}

// A higher priority task that calls CreateLaserTask while the main task allocates the laser task
static void OtherTaskCreates() noexcept
{
	Move::CreateLaserTask();
}

int main()
{
	Case("the laser task is allocated with the scheduler running and created", 1, nullptr, 1, 0);
	Case("a second call does not create another laser task", 2, nullptr, 1, 0);
	Case("another task that calls it meanwhile waits for the write lock and creates no second laser task", 1, OtherTaskCreates, 1, 0);
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
