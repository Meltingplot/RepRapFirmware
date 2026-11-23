// Stand-ins for the functions that alloc-check.py knows by name, so that the selftest programs link without a C library. See
// runtime.h. Without RESTORING_PORT they behave as the Cortex-M7 r0p1 port of upstream FreeRTOS V11.3.0, with RESTORING_PORT
// as that port of Meltingplot/FreeRTOS from PR #1 on, whose critical sections restore the mask that they found.

#include "runtime.h"
#include <new>

static volatile uint32_t suspended, critical, notified, available, waiting;
static char heap[256];
static size_t heapUsed;

#ifdef RESTORING_PORT

static volatile uint32_t basepriBeforeCritical;

// ulPortRaiseBASEPRI: raises BASEPRI with basepri_max inside cpsid i, restores PRIMASK after it and returns the BASEPRI that it
// found (ARM_CM7/r0p1 portmacro.h)
static inline __attribute__((always_inline)) uint32_t RaiseBasepri() noexcept
{
	const uint32_t saved = GetBasepri();
	const uint32_t flags = GetPrimask();
	DisableIrq();
	SetBasepriMax(0x60);
	SetPrimask(flags);
	return saved;
}

#endif

extern "C"
{
	__attribute__((noinline)) void vTaskSuspendAll() noexcept { suspended = suspended + 1; }

#ifdef RESTORING_PORT

	// The outermost vPortEnterCritical saves the BASEPRI that it found, and vPortExitCritical writes it back when the nesting
	// count drops to 0 (ARM_CM7/r0p1 port.c, vPortEnterCritical and vPortExitCritical)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		const uint32_t saved = RaiseBasepri();
		if (critical == 0)
		{
			basepriBeforeCritical = saved;
		}
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			SetBasepri(basepriBeforeCritical);
		}
	}

	// pdFALSE while the task holds a critical section other than the kernelNesting ones of the kernel function that asks, or a
	// mask of its own (ARM_CM7/r0p1 port.c, xPortTaskCanBlock)
	__attribute__((noinline)) long xPortTaskCanBlock(unsigned long kernelNesting) noexcept
	{
		if (critical != kernelNesting)
		{
			return 0;
		}
		const uint32_t basepri = (kernelNesting != 0) ? basepriBeforeCritical : GetBasepri();
		return (basepri == 0 && GetPrimask() == 0) ? 1 : 0;
	}

#else

	// As the Cortex-M7 r0p1 port of FreeRTOS: vPortEnterCritical wraps its msr basepri in cpsid i and cpsie i (portmacro.h:210-224),
	// and vPortExitCritical writes BASEPRI 0 when the nesting count drops to 0 (port.c:480-489)
	__attribute__((noinline)) void vPortEnterCritical() noexcept
	{
		DisableIrq();
		SetBasepri(0x60);
		EnableIrq();
		critical = critical + 1;
	}

	__attribute__((noinline)) void vPortExitCritical() noexcept
	{
		critical = critical - 1;
		if (critical == 0)
		{
			SetBasepri(0);
		}
	}

#endif

	// xTaskResumeAll works inside a critical section (tasks.c:4038-4162), as task notifications do
	__attribute__((noinline)) long xTaskResumeAll() noexcept
	{
		vPortEnterCritical();
		suspended = suspended - 1;
		vPortExitCritical();
		return 0;
	}

	__attribute__((noinline)) long xTaskGenericNotify() noexcept
	{
		vPortEnterCritical();
		notified = notified + 1;
		vPortExitCritical();
		return 1;
	}

	// A FromISR function raises BASEPRI and restores the value that it found (ulPortRaiseBASEPRI, vPortSetBASEPRI); on the
	// Cortex-M7 r0p1 of upstream FreeRTOS the raise ends with cpsie i (portmacro.h:228-247)
	__attribute__((noinline)) void vTaskGenericNotifyGiveFromISR() noexcept
	{
#ifdef RESTORING_PORT
		const uint32_t saved = RaiseBasepri();
#else
		const uint32_t saved = GetBasepri();
		DisableIrq();
		SetBasepri(0x60);
		EnableIrq();
#endif
		notified = notified + 1;
		SetBasepri(saved);
	}

	// Where a task blocks (tasks.c:5310). With RESTORING_PORT its trace hook asserts xPortTaskCanBlock(0) (FreeRTOSConfig.h of
	// Meltingplot/FreeRTOS, traceENTER_vTaskPlaceOnEventList)
	__attribute__((noinline)) void vTaskPlaceOnEventList() noexcept
	{
#ifdef RESTORING_PORT
		if (xPortTaskCanBlock(0) == 0)
		{
			for (;;) { }
		}
#endif
		waiting = waiting + 1;
	}

	// A semaphore take: a critical section that takes the semaphore if it is available or returns if the timeout is 0, then
	// a block with the scheduler suspended (queue.c:1659-1830, the block at :1806)
	__attribute__((noinline)) long xQueueSemaphoreTake(unsigned long ticksToWait) noexcept
	{
		vPortEnterCritical();
		const bool taken = (available != 0);
		if (taken)
		{
			available = available - 1;
		}
		vPortExitCritical();
		if (!taken && ticksToWait != 0)
		{
			vTaskSuspendAll();
			vTaskPlaceOnEventList();
			(void)xTaskResumeAll();
		}
		return taken;
	}

	__attribute__((noinline)) void *malloc(size_t n) noexcept
	{
		void *const p = (heapUsed + n <= sizeof(heap)) ? &heap[heapUsed] : nullptr;
		heapUsed += n;
		return p;
	}

	__attribute__((noinline)) void free(void *p) noexcept { if (p == nullptr) { heapUsed = 0; } }
}

void *operator new(size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
