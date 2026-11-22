// The host RTOS of shim/HostRtos.h. This file includes no eCv header, so it can use the standard library freely.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "shim/HostRtos.h"

constexpr unsigned int NumNotifyIndices = 8;						// configTASK_NOTIFICATION_ARRAY_ENTRIES in FreeRTOSConfig.h:161
constexpr uint32_t MaxSyscallBasePri = 0x50;						// any value other than 0 will do

struct HostTask
{
	std::mutex mutex;
	std::condition_variable cv;
	uint32_t counts[NumNotifyIndices] = { };
	bool waiting = false;
};

static std::mutex hostTasksMutex;
static std::deque<HostTask> hostTasks;								// owned here, so that nothing leaks
static std::recursive_mutex schedulerMutex;							// vTaskSuspendAll and the critical sections
static std::atomic<unsigned int> priorityQueries(0);

static thread_local StaticTask_t *currentTask = nullptr;
static thread_local bool inIsr = false;
static thread_local uint32_t basePri = 0;
static thread_local uint32_t priMask = 0;
static thread_local unsigned int suspendNesting = 0;
static thread_local unsigned int criticalNesting = 0;

static StaticTask_t *ToTask(TaskHandle_t h) noexcept
{
	return (h == nullptr) ? currentTask : reinterpret_cast<StaticTask_t *>(h);
}

static HostTask& Host(StaticTask_t *t) noexcept
{
	std::lock_guard<std::mutex> lock(hostTasksMutex);
	if (t->host == nullptr)
	{
		t->host = &hostTasks.emplace_back();
	}
	return *t->host;
}

TaskHandle_t xTaskGetCurrentTaskHandle() noexcept
{
	return reinterpret_cast<TaskHandle_t>(currentTask);
}

UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask) noexcept
{
	++priorityQueries;
	vPortEnterCritical();
	const UBaseType_t ret = ToTask(xTask)->uxPriority;
	vPortExitCritical();
	return ret;
}

UBaseType_t uxTaskBasePriorityGet(const TaskHandle_t xTask) noexcept
{
	++priorityQueries;
	vPortEnterCritical();
	const UBaseType_t ret = ToTask(xTask)->uxBasePriority;
	vPortExitCritical();
	return ret;
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) noexcept
{
	StaticTask_t *const t = ToTask(xTask);
	t->uxPriority = t->uxBasePriority = uxNewPriority;
}

BaseType_t xTaskGetSchedulerState() noexcept
{
	return (suspendNesting != 0) ? taskSCHEDULER_SUSPENDED : taskSCHEDULER_RUNNING;
}

void vTaskSuspendAll() noexcept
{
	schedulerMutex.lock();
	++suspendNesting;
}

BaseType_t xTaskResumeAll() noexcept
{
	vPortEnterCritical();
	--suspendNesting;
	vPortExitCritical();
	schedulerMutex.unlock();
	return pdFALSE;
}

void vPortEnterCritical() noexcept
{
	schedulerMutex.lock();
	basePri = MaxSyscallBasePri;
	++criticalNesting;
}

void vPortExitCritical() noexcept
{
	if (--criticalNesting == 0)
	{
		basePri = 0;
	}
	schedulerMutex.unlock();
}

BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify) noexcept
{
	HostTask& h = Host(ToTask(xTaskToNotify));
	{
		std::lock_guard<std::mutex> lock(h.mutex);
		++h.counts[uxIndexToNotify];
	}
	h.cv.notify_all();
	return pdTRUE;
}

uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait) noexcept
{
	HostTask& h = Host(currentTask);
	std::unique_lock<std::mutex> lock(h.mutex);
	h.waiting = true;
	if (xTicksToWait == portMAX_DELAY)
	{
		h.cv.wait(lock, [&h, uxIndexToWaitOn] { return h.counts[uxIndexToWaitOn] != 0; });
	}
	else
	{
		(void)h.cv.wait_for(lock, std::chrono::milliseconds(xTicksToWait), [&h, uxIndexToWaitOn] { return h.counts[uxIndexToWaitOn] != 0; });
	}
	h.waiting = false;
	const uint32_t ret = h.counts[uxIndexToWaitOn];
	if (ret != 0)
	{
		h.counts[uxIndexToWaitOn] = (xClearCountOnExit != pdFALSE) ? 0 : ret - 1;
	}
	return ret;
}

uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear) noexcept
{
	HostTask& h = Host(ToTask(xTask));
	std::lock_guard<std::mutex> lock(h.mutex);
	const uint32_t ret = h.counts[uxIndexToClear];
	h.counts[uxIndexToClear] &= ~ulBitsToClear;
	return ret;
}

void HostYield() noexcept
{
	std::this_thread::yield();
}

namespace HostRtos
{
	void SetCurrentTask(StaticTask_t *t) noexcept { currentTask = t; }
	void SetInInterrupt(bool b) noexcept { inIsr = b; }
	bool InInterrupt() noexcept { return inIsr; }
	void SetBasePri(uint32_t v) noexcept { basePri = v; }
	uint32_t GetBasePri() noexcept { return basePri; }
	void SetPriMask(uint32_t v) noexcept { priMask = v; }
	uint32_t GetPriMask() noexcept { return priMask; }
	unsigned int GetPriorityQueries() noexcept { return priorityQueries; }

	bool IsWaitingForNotification(StaticTask_t *t) noexcept
	{
		HostTask& h = Host(t);
		std::lock_guard<std::mutex> lock(h.mutex);
		return h.waiting;
	}
}

// End
