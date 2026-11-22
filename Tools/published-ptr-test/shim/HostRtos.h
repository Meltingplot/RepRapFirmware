// Host stand-in for the parts of FreeRTOS that RRFLibraries RTOSIface/RTOSIface.h declares, and that the ReadWriteLock code of
// RTOSIface.cpp and src/Platform/PublishedPtr.cpp call. host_rtos.cpp implements them on std::thread:
// - A task is a TaskBase (a StaticTask_t). A thread plays it after HostRtos::SetCurrentTask; xTaskGetCurrentTaskHandle returns it.
// - vTaskSuspendAll and the critical sections take one recursive mutex, so the lock code runs as in one critical section at a time.
// - Task notifications block the thread that takes them until another thread gives them.
// - BASEPRI, PRIMASK and "in an ISR" belong to the calling thread. As on the board, leaving the outermost critical section sets BASEPRI
//   to 0 (vPortExitCritical, FreeRTOS portable/GCC/ARM_CM7/r0p1/port.c:480-489, portENABLE_INTERRUPTS in portmacro.h:120). Every
//   priority query (uxTaskPriorityGet, uxTaskBasePriorityGet, tasks.c:2651-2668 and :2729-2750) and xTaskResumeAll (tasks.c:4038-4162)
//   goes through one.
//
// This header uses only <cstdint> and <cstddef>, because RTOSIface.h includes it after the eCv macros of ecv_duet3d.h.

#ifndef PUBLISHED_PTR_TEST_HOST_RTOS_H
#define PUBLISHED_PTR_TEST_HOST_RTOS_H

#include <cstdint>
#include <cstddef>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef void (*TaskFunction_t)(void *);

#define pdFALSE							((BaseType_t)0)
#define pdTRUE							((BaseType_t)1)
#define portMAX_DELAY					((TickType_t)0xFFFFFFFFu)
#define configMAX_PRIORITIES			(8)							// as in FreeRTOSConfig.h:123
#define configRUN_TIME_COUNTER_TYPE		uint32_t
#define configSTACK_DEPTH_TYPE			uint32_t

// As in FreeRTOS task.h:282-284
#define taskSCHEDULER_SUSPENDED			((BaseType_t)0)
#define taskSCHEDULER_NOT_STARTED		((BaseType_t)1)
#define taskSCHEDULER_RUNNING			((BaseType_t)2)

struct HostTask;													// the notification state of a task, see host_rtos.cpp

struct StaticTask_t
{
	UBaseType_t uxPriority = 0;										// the priority the task runs at, including one that it has inherited
	UBaseType_t uxBasePriority = 0;									// the priority it was given
	HostTask *host = nullptr;
};

struct StaticSemaphore_t { void *dummy[4]; };
struct StaticQueue_t { void *dummy[4]; };
struct tskTaskControlBlock;
typedef tskTaskControlBlock *TaskHandle_t;
struct QueueDefinition;
typedef QueueDefinition *QueueHandle_t;

// What the test calls, implemented in host_rtos.cpp
TaskHandle_t xTaskGetCurrentTaskHandle() noexcept;
UBaseType_t uxTaskPriorityGet(const TaskHandle_t xTask) noexcept;
UBaseType_t uxTaskBasePriorityGet(const TaskHandle_t xTask) noexcept;
void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) noexcept;
BaseType_t xTaskGetSchedulerState() noexcept;
void vTaskSuspendAll() noexcept;
BaseType_t xTaskResumeAll() noexcept;
void vPortEnterCritical() noexcept;
void vPortExitCritical() noexcept;
BaseType_t xTaskNotifyGiveIndexed(TaskHandle_t xTaskToNotify, UBaseType_t uxIndexToNotify) noexcept;
uint32_t ulTaskNotifyTakeIndexed(UBaseType_t uxIndexToWaitOn, BaseType_t xClearCountOnExit, TickType_t xTicksToWait) noexcept;
uint32_t ulTaskNotifyValueClearIndexed(TaskHandle_t xTask, UBaseType_t uxIndexToClear, uint32_t ulBitsToClear) noexcept;
void HostYield() noexcept;

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

// Controls of the test, for the calling thread unless the task is given
namespace HostRtos
{
	void SetCurrentTask(StaticTask_t *t) noexcept;			// the calling thread plays this task from now on
	void SetInInterrupt(bool b) noexcept;							// the calling thread plays an ISR (CoreN2G inInterrupt)
	bool InInterrupt() noexcept;
	void SetBasePri(uint32_t v) noexcept;							// CMSIS __set_BASEPRI
	uint32_t GetBasePri() noexcept;
	void SetPriMask(uint32_t v) noexcept;
	uint32_t GetPriMask() noexcept;
	unsigned int GetPriorityQueries() noexcept;						// calls of uxTaskPriorityGet and uxTaskBasePriorityGet so far, all threads
	bool IsWaitingForNotification(StaticTask_t *t) noexcept;	// true while the task waits in ulTaskNotifyTakeIndexed
}

#endif
