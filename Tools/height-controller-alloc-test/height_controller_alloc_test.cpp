// Host test of the allocation of the height controller task by M951 (HeightController::Configure in
// src/Movement/HeightControl/HeightController.cpp), compiled from the real text of the function that the Makefile extracts.
//
// Task<> has no operator new of its own (RRFLibraries src/RTOSIface/RTOSIface.h), so the task comes from ::operator new and so from
// RRF's malloc. That takes the malloc mutex only while the scheduler is running (GetMallocMutex in src/Platform/Tasks.cpp), so a
// malloc with the scheduler suspended runs without it, while a task of the same or a lower priority may have been preempted inside
// malloc. M951 runs in the task of the G-code channel, the main task. Up to 3.7-dev de1bfe598, Configure allocated the task inside
// its TaskCriticalSectionLocker, which suspends the scheduler.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter, Task<> with a stand-in whose Create records the
// call, and ::operator new and delete with the host heap. No allocation or free of the task may happen with the scheduler suspended.
// It also checks what M951 does: H creates the task once, a second M951 does not create another, without a sensor no task is created,
// and M951 without parameters reports the settings. M951 runs only in the main task, so no other task can create the task while this
// one allocates its own.

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#define _ecv_null
#define THROWS(...)

enum class GCodeResult { notFinished, ok, error };

namespace TaskPriority
{
	constexpr unsigned int HeightFollowingPriority = 4;		// src/Platform/TaskPriorities.h
}

static unsigned int suspended = 0;							// uxSchedulerSuspended
static bool checking = false;									// true while a case runs Configure
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

// Just what Configure uses of TaskBase and Task (RRFLibraries RTOSIface.h). As there, a task that was never created has a zero taskId,
// and the destructor of such a task does nothing (TaskBase::TerminateAndUnlink in RTOSIface.cpp).
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

extern "C" void HeightControllerTaskStart(void *) noexcept { }

// Just what Configure uses of GCodeBuffer and StringRef: the parameters of one command
class GCodeBuffer
{
public:
	explicit GCodeBuffer(const char *p_params) noexcept : params(p_params) { }

	bool Seen(char c) noexcept
	{
		const char *const p = strchr(params, c);
		next = (p != nullptr) ? p + 1 : nullptr;
		return p != nullptr;
	}

	float GetPositiveFValue() noexcept { return strtof(next, nullptr); }

	void TryGetUIValue(char c, uint32_t& val, bool& seen) noexcept
	{
		if (Seen(c)) { val = strtoul(next, nullptr, 10); seen = true; }
	}

	void TryGetFValue(char c, float& val, bool& seen) noexcept
	{
		if (Seen(c)) { val = strtof(next, nullptr); seen = true; }
	}

	void TryGetFloatArray(char c, size_t numVals, float vals[], bool& seen, bool doPad) noexcept
	{
		(void)doPad;
		if (Seen(c))
		{
			char *end;
			vals[0] = strtof(next, &end);
			vals[numVals - 1] = (*end == ':') ? strtof(end + 1, nullptr) : vals[0];
			seen = true;
		}
	}

private:
	const char *params;
	const char *next = nullptr;
};

class StringRef
{
public:
	void copy(const char *s) const noexcept { text = s; }
	int printf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		char buf[200];
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(buf, sizeof(buf), fmt, vargs);
		va_end(vargs);
		text = buf;
		return ret;
	}

	mutable std::string text;
};

// The members of HeightController in src/Movement/HeightControl/HeightController.h that Configure uses
class HeightController
{
public:
	GCodeResult Configure(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	static constexpr unsigned int HeightControllerTaskStackWords = 100;
	Task<HeightControllerTaskStackWords> *heightControllerTask = nullptr;
	int sensorNumber = -1;
	uint32_t sampleInterval = 200;
	float pidP = 1.0, configuredPidI = 0.0, configuredPidD = 0.0;
	float zMin = 5.0, zMax = 10.0;

	enum class PidState : uint8_t { stopped, starting, running };
	volatile PidState state = PidState::running;

private:
	void CalcDerivedValues() noexcept { }
};

#include "HeightController.inc"

// The host heap, with the check. Only the allocations of a height controller task are counted, by their size.
void *operator new(size_t sz)
{
	const bool isTask = checking && sz == sizeof(Task<HeightController::HeightControllerTaskStackWords>);
	if (isTask)
	{
		if (suspended != 0)
		{
			failures.push_back("the height controller task was allocated with the scheduler suspended");
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
	if (checking && p != nullptr && sz == sizeof(Task<HeightController::HeightControllerTaskStackWords>))
	{
		if (suspended != 0)
		{
			failures.push_back("a height controller task was freed with the scheduler suspended");
		}
		++taskFrees;
	}
	free(p);
}

static unsigned int passed = 0, failed = 0;

// Run M951 with each of 'commands' on a new HeightController and check the number of tasks created, allocated and freed
static void Case(const char *name, std::vector<const char *> commands, unsigned int expectedCreates, unsigned int expectedAllocations,
					unsigned int expectedFrees, const char *expectedReply) noexcept
{
	HeightController hc;
	failures.clear();
	taskAllocations = taskFrees = creates = 0;
	StringRef reply;
	checking = true;
	for (const char *cmd : commands)
	{
		GCodeBuffer gb(cmd);
		(void)hc.Configure(gb, reply);
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
	if (creates != expectedCreates || (expectedCreates != 0 && (hc.heightControllerTask == nullptr || hc.state != HeightController::PidState::stopped)))
	{
		why += (why.empty() ? "" : "; ") + std::string("the task was created ") + std::to_string(creates) + " times";
	}
	if (taskAllocations != expectedAllocations || taskFrees != expectedFrees)
	{
		why += (why.empty() ? "" : "; ") + std::string("allocations ") + std::to_string(taskAllocations) + ", frees " + std::to_string(taskFrees)
				+ ", expected " + std::to_string(expectedAllocations) + " and " + std::to_string(expectedFrees);
	}
	if (expectedReply != nullptr && reply.text != expectedReply)
	{
		why += (why.empty() ? "" : "; ") + std::string("reply \"") + reply.text + "\", expected \"" + expectedReply + "\"";
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
	delete hc.heightControllerTask;
}

int main()
{
	Case("M951 H1 allocates the task with the scheduler running and creates it", { "H1" }, 1, 1, 0, nullptr);
	Case("a second M951 does not create another task", { "H1", "H1 P2" }, 1, 1, 0, nullptr);
	Case("M951 P2 without a sensor creates no task", { "P2" }, 0, 0, 0, nullptr);
	Case("M951 without parameters and without a sensor reports that", { "" }, 0, 0, 0, "Height controller is not configured");
	Case("M951 without parameters reports the settings", { "H2 F10 Z1:4", "" }, 1, 1, 0,
			"Height controller uses sensor 2, frequency 10.0, P1.0 I0.0 D0.0, Z1.0 to 4.0");
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
