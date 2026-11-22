// Host stand-in for the parts of FreeRTOS that RRFLibraries RTOSIface/RTOSIface.h declares and that the code under test calls.
// host_rtos.cpp implements them as ONE simulated processor (configNUMBER_OF_CORES == 1): every task is a thread, exactly one task runs at a
// time, and a context switch happens only where FreeRTOS would switch on the board:
//  - TestInterrupt(task): an interrupt makes a task ready (the tick waking a delayed task, an ISR giving a notification). If that task has a
//    higher priority than the running one, the switch happens on return from the interrupt (PendSV), unless the scheduler is suspended:
//    then it happens in xTaskResumeAll (FreeRTOS tasks.c:4049-4110, xPendingReadyList).
//  - a task blocks: it takes a mutex that another task holds (priority inheritance raises the holder, queue.c:1795), or it waits for a notification.
//  - a mutex or notification is given to a task with a higher priority (queue.c xQueueGenericSend, tasks.c xTaskGenericNotify).
// vTaskSuspendAll and xTaskGetSchedulerState are the real FreeRTOS text (tasks.c), extracted into gen/ by the Makefile.
//
// This header uses only <cstdint> and <cstddef>, because RTOSIface.h includes it after the eCv macros of ecv_duet3d.h.

#ifndef TEST_HOST_RTOS_H
#define TEST_HOST_RTOS_H

#include <cstdint>
#include <cstddef>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void (*TaskFunction_t)(void *);

#define pdFALSE							((BaseType_t)0)
#define pdTRUE							((BaseType_t)1)
#define pdPASS							pdTRUE
#define pdFAIL							pdFALSE
#define portMAX_DELAY					((TickType_t)0xFFFFFFFFu)
#define configMAX_PRIORITIES			(8)							// FreeRTOSConfig.h:123
#define configNUMBER_OF_CORES			(1)							// FreeRTOS.h default
#define configRUN_TIME_COUNTER_TYPE		uint32_t
#define configSTACK_DEPTH_TYPE			uint32_t

// As in FreeRTOS task.h:283-285
#define taskSCHEDULER_SUSPENDED			((BaseType_t)0)
#define taskSCHEDULER_NOT_STARTED		((BaseType_t)1)
#define taskSCHEDULER_RUNNING			((BaseType_t)2)

extern "C" [[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept;
#define configASSERT(x)					if ((x) == 0) vAssertCalled(__LINE__, __FILE__)	// FreeRTOSConfig.h:437

struct HostTaskState;

// The task control block. TaskBase (RTOSIface.h:170) derives from it.
struct StaticTask_t
{
	UBaseType_t uxPriority = 0;										// the priority the task runs at, including an inherited one
	UBaseType_t uxBasePriority = 0;									// the priority it was given
	HostTaskState *host = nullptr;
};

// The mutex control block. Mutex (RTOSIface.h:98) derives from it.
struct StaticSemaphore_t
{
	StaticTask_t *holder = nullptr;
	UBaseType_t recursiveCount = 0;
	bool created = false;
};
struct StaticQueue_t { void *dummy[4]; };
struct tskTaskControlBlock;
typedef tskTaskControlBlock *TaskHandle_t;
struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle() noexcept;
UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask) noexcept;
void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) noexcept;
BaseType_t xTaskGetSchedulerState();								// no noexcept: host_rtos.cpp compiles the real tasks.c text of these two
void vTaskSuspendAll();
BaseType_t xTaskResumeAll() noexcept;
void vPortEnterCritical() noexcept;
void vPortExitCritical() noexcept;
BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify) noexcept;
uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait) noexcept;
uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear) noexcept;
void HostYield() noexcept;

// The recursive mutex functions that semphr.h maps xSemaphore...Recursive onto (queue.c:759-870, 1659-1850)
QueueHandle_t HostCreateRecursiveMutex(StaticSemaphore_t *p) noexcept;
BaseType_t HostTakeMutexRecursive(QueueHandle_t h, TickType_t xTicksToWait) noexcept;
BaseType_t HostGiveMutexRecursive(QueueHandle_t h) noexcept;
TaskHandle_t HostGetMutexHolder(QueueHandle_t h) noexcept;

#define xSemaphoreCreateRecursiveMutexStatic(_p)	HostCreateRecursiveMutex(_p)
#define xSemaphoreTakeRecursive(_h, _t)				HostTakeMutexRecursive((_h), (_t))
#define xSemaphoreGiveRecursive(_h)					HostGiveMutexRecursive(_h)
#define xSemaphoreGetMutexHolder(_h)				HostGetMutexHolder(_h)
#define vSemaphoreDelete(_h)						((void)(_h))

#define taskENTER_CRITICAL()			vPortEnterCritical()
#define taskEXIT_CRITICAL()				vPortExitCritical()
#define taskYIELD()						HostYield()
#define portYIELD_FROM_ISR(_x)			((void)(_x))

// Declared because RTOSIface.h uses them in inline functions and templates that the test never calls, so they have no definition
void vTaskSuspend(TaskHandle_t xTaskToSuspend) noexcept;
void vTaskResume(TaskHandle_t xTaskToResume) noexcept;
void vTaskDelete(TaskHandle_t xTaskToDelete) noexcept;
const StackType_t *pxTaskGetCurrentStackBase() noexcept;
TaskHandle_t xTaskCreateStatic(TaskFunction_t pxTaskCode, const char *pcName, uint32_t ulStackDepth, void *pvParameters,
								UBaseType_t uxPriority, StackType_t *puxStackBuffer, StaticTask_t *pxTaskBuffer) noexcept;
QueueHandle_t xQueueCreateStatic(UBaseType_t uxQueueLength, UBaseType_t uxItemSize, uint8_t *pucQueueStorage, StaticQueue_t *pxStaticQueue) noexcept;
BaseType_t xQueueSendToBack(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait) noexcept;
BaseType_t xQueueSendToFront(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait) noexcept;
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait) noexcept;
void vTaskNotifyGiveIndexedFromISR(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify, BaseType_t *pxHigherPriorityTaskWoken) noexcept;

// CMSIS, for the running task (Tasks.cpp:117 reads them)
uint32_t __get_BASEPRI() noexcept;
uint32_t __get_PRIMASK() noexcept;

// Controls of the simulated processor, used by lock_record_alloc_test.cpp
namespace HostRtos
{
	void CreateTask(StaticTask_t *t, const char *name, UBaseType_t priority, void (*fn)(void *), void *arg) noexcept;
	void StartScheduler() noexcept;									// runs the tasks until none is ready, then returns
	const char *Name(const StaticTask_t *t) noexcept;
	StaticTask_t *Current() noexcept;
	void Interrupt(StaticTask_t *woken) noexcept;					// an ISR makes 'woken' ready and requests a context switch
	void Log(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
	unsigned int Suspended() noexcept;							// uxSchedulerSuspended
}

#endif
