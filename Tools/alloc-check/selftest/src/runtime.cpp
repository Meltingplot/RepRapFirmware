// Stand-ins for the functions that alloc-check.py knows by name, so that the selftest programs link without a C library. See
// runtime.h.

#include "runtime.h"
#include <new>

static volatile uint32_t suspended, critical, notified;
static char heap[256];
static size_t heapUsed;

extern "C"
{
	__attribute__((noinline)) void vTaskSuspendAll() noexcept { suspended = suspended + 1; }

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
	// Cortex-M7 r0p1 the raise ends with cpsie i (portmacro.h:228-247)
	__attribute__((noinline)) void vTaskGenericNotifyGiveFromISR() noexcept
	{
		const uint32_t saved = GetBasepri();
		DisableIrq();
		SetBasepri(0x60);
		EnableIrq();
		notified = notified + 1;
		SetBasepri(saved);
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
