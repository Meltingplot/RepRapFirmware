// Host test of the allocation of the laser task by Move::CreateLaserTask (src/Movement/Move.cpp), compiled from the real text of the
// function and of the definition of Move::laserTask that the Makefile extracts.
//
// Task<> has no operator new of its own (RRFLibraries src/RTOSIface/RTOSIface.h), so the laser task comes from ::operator new and
// so from RRF's malloc. That takes the malloc mutex only while the scheduler is running (GetMallocMutex in src/Platform/Tasks.cpp),
// so a malloc with the scheduler suspended runs without it, while a task of the same or a lower priority may have been preempted
// inside malloc. CreateLaserTask runs in the main task, when laser mode or IOBits is selected (M452, M670), a scanning Z probe is
// configured (EndstopsManager.cpp), M571 is used (Platform.cpp) or extruder heater feedforward is configured (Tool.cpp). Up to
// 3.7-dev de1bfe598 it allocated the task inside its TaskCriticalSectionLocker, which suspends the scheduler.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter, Task<> with a stand-in whose Create records
// the call, and ::operator new and delete with the host heap. No allocation or free may happen with the scheduler suspended. It also
// checks what CreateLaserTask does: it creates the task once and a second call does nothing. Every caller runs in the main task, so
// no other task can create the laser task while this one allocates its own.

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

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

typedef void (*TaskFunction_t)(void *);

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
};

#include "CreateLaserTask.inc"

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
	return malloc(sz);
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

static void Case(const char *name, unsigned int calls, unsigned int expectedAllocations, unsigned int expectedFrees) noexcept
{
	failures.clear();
	taskAllocations = taskFrees = creates = 0;
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

int main()
{
	Case("the laser task is allocated with the scheduler running and created", 1, 1, 0);
	Case("a second call does not create another laser task", 2, 1, 0);
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
