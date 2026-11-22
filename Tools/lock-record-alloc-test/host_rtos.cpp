// The simulated single-core FreeRTOS of shim/HostRtos.h. One thread per task; the processor is handed from thread to thread under
// 'cpu', so exactly one task runs at a time and the order of events is the same on every run.

#include <pthread.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "shim/HostRtos.h"

constexpr unsigned int NumNotifyIndices = 8;						// configTASK_NOTIFICATION_ARRAY_ENTRIES, FreeRTOSConfig.h:161
constexpr unsigned int MaxTasks = 8;

struct HostTaskState
{
	enum State { Ready, BlockedOnMutex, BlockedOnNotification, Finished };
	const char *name;
	void (*fn)(void *);
	void *arg;
	pthread_t thread;
	State state;
	uint32_t notify[NumNotifyIndices];
	UBaseType_t waitIndex;
	StaticSemaphore_t *waitingOn;
};

static HostTaskState hostStates[MaxTasks];
static StaticTask_t *tasks[MaxTasks];
static unsigned int numTasks = 0;
static pthread_mutex_t cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cpuChanged = PTHREAD_COND_INITIALIZER;
static StaticTask_t *running = nullptr;								// the task that has the processor
static bool yieldPending = false;									// xYieldPendings[0] in tasks.c: a switch held while the scheduler is suspended
static unsigned int criticalNesting = 0;							// FreeRTOS uxCriticalNesting (port.c)
static unsigned int logSequence = 0;

// The FreeRTOS scheduler state, as declared in tasks.c:506 and :531
#define PRIVILEGED_DATA
static volatile BaseType_t xSchedulerRunning = pdFALSE;
static volatile UBaseType_t uxSchedulerSuspended = ( UBaseType_t ) 0U;

// What the extracted tasks.c text needs (FreeRTOS.h defaults, portmacro.h:259 for the barrier on Cortex-M7)
#define traceENTER_vTaskSuspendAll()
#define traceRETURN_vTaskSuspendAll()
#define traceENTER_xTaskGetSchedulerState()
#define traceRETURN_xTaskGetSchedulerState(x)
#define portSOFTWARE_BARRIER()
#define portMEMORY_BARRIER()			__asm volatile ( "" ::: "memory" )
#define mtCOVERAGE_TEST_MARKER()

// The real vTaskSuspendAll (tasks.c:3848-) and xTaskGetSchedulerState (tasks.c:6610-6643), extracted by the Makefile
#include "gen/vTaskSuspendAll.inc"
#include "gen/xTaskGetSchedulerState.inc"

namespace HostRtos
{
	void Log(const char *fmt, ...) noexcept
	{
		va_list vargs;
		va_start(vargs, fmt);
		printf("  [%03u] ", ++logSequence);
		vprintf(fmt, vargs);
		printf("\n");
		va_end(vargs);
		fflush(stdout);
	}

	const char *Name(const StaticTask_t *t) noexcept { return (t == nullptr) ? "(none)" : t->host->name; }
	StaticTask_t *Current() noexcept { return running; }
	unsigned int Suspended() noexcept { return uxSchedulerSuspended; }
}

using HostRtos::Log;
using HostRtos::Name;

static StaticTask_t *HighestReady() noexcept
{
	StaticTask_t *best = nullptr;
	for (unsigned int i = 0; i < numTasks; ++i)
	{
		StaticTask_t *const t = tasks[i];
		if (t->host->state == HostTaskState::Ready && (best == nullptr || t->uxPriority > best->uxPriority))
		{
			best = t;
		}
	}
	return best;
}

// Give the processor to the highest-priority ready task. Called with 'cpu' locked by the thread of the running task; returns when that task
// runs again. The running task keeps the processor if it is ready and no ready task has a higher priority (no time slicing is modelled).
static void Reschedule() noexcept
{
	StaticTask_t *const self = running;
	StaticTask_t *const next = HighestReady();
	if (self != nullptr && self->host->state == HostTaskState::Ready && (next == nullptr || next->uxPriority <= self->uxPriority))
	{
		return;
	}
	running = next;
	Log("context switch %s (priority %lu) -> %s (priority %lu)", Name(self), (self == nullptr) ? 0ul : self->uxPriority,
			Name(next), (next == nullptr) ? 0ul : next->uxPriority);
	pthread_cond_broadcast(&cpuChanged);
	while (running != self)
	{
		pthread_cond_wait(&cpuChanged, &cpu);
	}
}

// A task that a higher-priority one has just readied: switch now, or when the scheduler is resumed (tasks.c:4049-4110)
static void SwitchIfHigher(StaticTask_t *woken) noexcept
{
	if (running != nullptr && woken->uxPriority > running->uxPriority)
	{
		if (uxSchedulerSuspended == 0)
		{
			Reschedule();
		}
		else
		{
			yieldPending = true;
			Log("the scheduler is suspended, so the switch to %s waits for xTaskResumeAll", Name(woken));
		}
	}
}

static void *TaskThread(void *p) noexcept
{
	StaticTask_t *const t = static_cast<StaticTask_t *>(p);
	pthread_mutex_lock(&cpu);
	while (running != t)
	{
		pthread_cond_wait(&cpuChanged, &cpu);
	}
	pthread_mutex_unlock(&cpu);

	t->host->fn(t->host->arg);

	pthread_mutex_lock(&cpu);
	t->host->state = HostTaskState::Finished;
	Log("%s has finished", Name(t));
	running = HighestReady();
	pthread_cond_broadcast(&cpuChanged);
	pthread_mutex_unlock(&cpu);
	return nullptr;
}

namespace HostRtos
{
	void CreateTask(StaticTask_t *t, const char *name, UBaseType_t priority, void (*fn)(void *), void *arg) noexcept
	{
		HostTaskState *const h = &hostStates[numTasks];
		memset(h, 0, sizeof(*h));
		h->name = name;
		h->fn = fn;
		h->arg = arg;
		h->state = HostTaskState::Ready;
		t->host = h;
		t->uxPriority = t->uxBasePriority = priority;
		tasks[numTasks++] = t;
		pthread_create(&h->thread, nullptr, TaskThread, t);
	}

	// Run the tasks until none is ready. Like vTaskStartScheduler, it starts the highest-priority ready task.
	void StartScheduler() noexcept
	{
		xSchedulerRunning = pdTRUE;
		pthread_mutex_lock(&cpu);
		running = HighestReady();
		Log("scheduler started, %s runs", Name(running));
		pthread_cond_broadcast(&cpuChanged);
		while (running != nullptr)
		{
			pthread_cond_wait(&cpuChanged, &cpu);
		}
		pthread_mutex_unlock(&cpu);
		xSchedulerRunning = pdFALSE;
		for (unsigned int i = 0; i < numTasks; ++i)
		{
			if (tasks[i]->host->state != HostTaskState::Finished)
			{
				Log("%s is still blocked when no task is ready", Name(tasks[i]));
			}
		}
	}

	// An interrupt at this point of the running task makes 'woken' ready: the tick unblocks a delayed task (tasks.c:4753-4866) or an ISR gives
	// it a notification (RTOSIface.cpp:203-211). The switch happens on return from the interrupt if 'woken' has the higher priority (PendSV,
	// port.c:550-567), because the running task is preemptible at every instruction (configUSE_PREEMPTION 1, FreeRTOSConfig.h:77).
	void Interrupt(StaticTask_t *woken) noexcept
	{
		pthread_mutex_lock(&cpu);
		Log("INTERRUPT while %s runs: it makes %s (priority %lu) ready", Name(running), Name(woken), woken->uxPriority);
		if (woken->host->state == HostTaskState::BlockedOnNotification)
		{
			++woken->host->notify[woken->host->waitIndex];
			woken->host->state = HostTaskState::Ready;
		}
		SwitchIfHigher(woken);
		pthread_mutex_unlock(&cpu);
	}
}

TaskHandle_t xTaskGetCurrentTaskHandle() noexcept
{
	return reinterpret_cast<TaskHandle_t>(running);
}

UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask) noexcept
{
	return ((xTask == nullptr) ? running : reinterpret_cast<StaticTask_t *>(xTask))->uxPriority;
}

void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) noexcept
{
	StaticTask_t *const t = (xTask == nullptr) ? running : reinterpret_cast<StaticTask_t *>(xTask);
	t->uxPriority = t->uxBasePriority = uxNewPriority;
}

// Stand-in for xTaskResumeAll (tasks.c:4014-4162): the decrement of tasks.c:4044-4046, then the switch that a task readied while the scheduler
// was suspended asks for (tasks.c:4049-4110)
BaseType_t xTaskResumeAll() noexcept
{
	configASSERT( uxSchedulerSuspended != 0U );						// tasks.c:4044
	uxSchedulerSuspended = ( UBaseType_t ) ( uxSchedulerSuspended - 1U );	// tasks.c:4046
	BaseType_t xAlreadyYielded = pdFALSE;
	if (uxSchedulerSuspended == 0U && yieldPending)
	{
		pthread_mutex_lock(&cpu);
		yieldPending = false;
		Log("xTaskResumeAll: performing the held switch");
		Reschedule();
		pthread_mutex_unlock(&cpu);
		xAlreadyYielded = pdTRUE;
	}
	return xAlreadyYielded;
}

void vPortEnterCritical() noexcept { ++criticalNesting; }
void vPortExitCritical() noexcept { --criticalNesting; }
uint32_t __get_BASEPRI() noexcept { return (criticalNesting != 0) ? 0x50 : 0; }
uint32_t __get_PRIMASK() noexcept { return 0; }

void HostYield() noexcept { }

// Recursive mutex, as xQueueTakeMutexRecursive / xQueueGiveMutexRecursive (queue.c:759-870) with the blocking and priority inheritance of
// xQueueSemaphoreTake (queue.c:1659-1850) and the disinheritance of xTaskPriorityDisinherit
QueueHandle_t HostCreateRecursiveMutex(StaticSemaphore_t *p) noexcept
{
	p->holder = nullptr;
	p->recursiveCount = 0;
	p->created = true;
	return reinterpret_cast<QueueHandle_t>(p);
}

BaseType_t HostTakeMutexRecursive(QueueHandle_t h, TickType_t xTicksToWait) noexcept
{
	StaticSemaphore_t *const s = reinterpret_cast<StaticSemaphore_t *>(h);
	StaticTask_t *const me = running;
	if (s->holder == me)
	{
		++s->recursiveCount;										// queue.c:833-840
		return pdPASS;
	}
	configASSERT( !( ( xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED ) && ( xTicksToWait != 0 ) ) );	// queue.c:1682
	pthread_mutex_lock(&cpu);
	while (s->holder != nullptr)
	{
		if (s->holder->uxPriority < me->uxPriority)
		{
			Log("priority inheritance: %s raised from %lu to %lu (queue.c:1795)", Name(s->holder), s->holder->uxPriority, me->uxPriority);
			s->holder->uxPriority = me->uxPriority;
		}
		Log("%s blocks on the mutex that %s holds", Name(me), Name(s->holder));
		me->host->state = HostTaskState::BlockedOnMutex;
		me->host->waitingOn = s;
		Reschedule();
	}
	s->holder = me;
	s->recursiveCount = 1;
	pthread_mutex_unlock(&cpu);
	return pdPASS;
}

BaseType_t HostGiveMutexRecursive(QueueHandle_t h) noexcept
{
	StaticSemaphore_t *const s = reinterpret_cast<StaticSemaphore_t *>(h);
	StaticTask_t *const me = running;
	if (s->holder != me)
	{
		return pdFAIL;
	}
	if (--s->recursiveCount != 0)
	{
		return pdPASS;
	}
	pthread_mutex_lock(&cpu);
	s->holder = nullptr;
	if (me->uxPriority != me->uxBasePriority)
	{
		Log("%s returns to priority %lu", Name(me), me->uxBasePriority);
		me->uxPriority = me->uxBasePriority;
	}
	StaticTask_t *waiter = nullptr;
	for (unsigned int i = 0; i < numTasks; ++i)
	{
		StaticTask_t *const t = tasks[i];
		if (t->host->state == HostTaskState::BlockedOnMutex && t->host->waitingOn == s && (waiter == nullptr || t->uxPriority > waiter->uxPriority))
		{
			waiter = t;
		}
	}
	if (waiter != nullptr)
	{
		waiter->host->state = HostTaskState::Ready;
		waiter->host->waitingOn = nullptr;
		SwitchIfHigher(waiter);
	}
	pthread_mutex_unlock(&cpu);
	return pdPASS;
}

TaskHandle_t HostGetMutexHolder(QueueHandle_t h) noexcept
{
	return reinterpret_cast<TaskHandle_t>(reinterpret_cast<StaticSemaphore_t *>(h)->holder);
}

uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait) noexcept
{
	StaticTask_t *const me = running;
	pthread_mutex_lock(&cpu);
	while (me->host->notify[uxIndexToWaitOn] == 0)
	{
		me->host->state = HostTaskState::BlockedOnNotification;
		me->host->waitIndex = uxIndexToWaitOn;
		Reschedule();
	}
	const uint32_t ret = me->host->notify[uxIndexToWaitOn];
	me->host->notify[uxIndexToWaitOn] = (xClearCountOnExit != pdFALSE) ? 0 : ret - 1;
	pthread_mutex_unlock(&cpu);
	return ret;
}

BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify) noexcept
{
	StaticTask_t *const t = reinterpret_cast<StaticTask_t *>(xTaskToNotify);
	pthread_mutex_lock(&cpu);
	++t->host->notify[uxIndexToNotify];
	if (t->host->state == HostTaskState::BlockedOnNotification && t->host->waitIndex == uxIndexToNotify)
	{
		t->host->state = HostTaskState::Ready;
		SwitchIfHigher(t);
	}
	pthread_mutex_unlock(&cpu);
	return pdPASS;
}

uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear) noexcept
{
	StaticTask_t *const t = (xTask == nullptr) ? running : reinterpret_cast<StaticTask_t *>(xTask);
	const uint32_t ret = t->host->notify[uxIndexToClear];
	t->host->notify[uxIndexToClear] &= ~ulBitsToClear;
	return ret;
}

// End
