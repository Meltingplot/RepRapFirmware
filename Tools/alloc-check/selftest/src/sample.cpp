// Functions with a known answer for alloc-check.py, see ../../README.md. "make selftest" compiles and links this file with
// runtime.cpp, with the firmware's compiler, -O2 and -g, and compares the findings with ../expected.txt (heap) and
// ../expected-unmask.txt (--unmask). It builds them a second time with RESTORING_PORT, whose critical sections restore the mask
// (runtime.cpp), and compares the findings of that build with the same ../expected.txt and with
// ../expected-unmask-restoring.txt. Each function says whether the check must find it.

#include "runtime.h"

// An inline locker, as TaskCriticalSectionLocker in RRFLibraries RTOSIface.h: the calls of vTaskSuspendAll and xTaskResumeAll
// end up in the function that uses it
class Locker
{
public:
	Locker() noexcept { vTaskSuspendAll(); }
	~Locker() noexcept { (void)xTaskResumeAll(); }
};

// Out-of-line functions that open and close a region for their caller
__attribute__((noinline)) void EnterRegion() noexcept { vTaskSuspendAll(); }
__attribute__((noinline)) void LeaveRegion() noexcept { (void)xTaskResumeAll(); }

// A function that allocates; a call to it inside a region is found
__attribute__((noinline)) void *Helper(unsigned int n) noexcept { return malloc(n); }

int *volatile sink;
void *volatile rawSink;
volatile int selector;
void (*volatile callback)() noexcept;

// Found: operator new between vTaskSuspendAll and xTaskResumeAll
void SuspendedNew() noexcept
{
	vTaskSuspendAll();
	sink = new int(1);
	(void)xTaskResumeAll();
}

// Found: malloc inside the inline locker
void LockerMalloc() noexcept
{
	Locker lock;
	rawSink = malloc(16);
}

// Found: free between the out-of-line functions that open and close the region
void OutOfLineFree(void *p) noexcept
{
	EnterRegion();
	free(p);
	LeaveRegion();
}

// Found: a function that allocates, called inside a FreeRTOS critical section (taskENTER_CRITICAL)
void CallsHelperInCritical() noexcept
{
	vPortEnterCritical();
	rawSink = Helper(8);
	vPortExitCritical();
}

// Found: operator new with interrupts disabled, as AtomicCriticalSectionLocker (CoreN2G CoreIO.h) does it
void MaskedNew() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	sink = new int(2);
	SetPrimask(flags);
}

// Found: malloc with BASEPRI raised, as BasePriorityBooster (CoreN2G CoreIO.h) does it
void BoostedMalloc() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0x50);
	rawSink = malloc(4);
	SetBasepri(old);
}

// Found: a call through a function pointer inside a region, whose target the check cannot see
void SuspendedCallback() noexcept
{
	Locker lock;
	callback();
}

// Found, keyed with the file name: a static function
static __attribute__((noinline)) void StaticSuspended() noexcept
{
	Locker lock;
	rawSink = Helper(32);
}

// Found: one case of a switch (a jump table) inside a region allocates
void SwitchInRegion() noexcept
{
	Locker lock;
	switch (selector)
	{
	case 0: sink = nullptr; break;
	case 1: rawSink = Helper(1); break;
	case 2: selector = 5; break;
	case 3: selector = 7; break;
	case 4: selector = 9; break;
	case 5: selector = 11; break;
	default: break;
	}
}

// Found: an exception handler in the vector table that allocates
extern "C" void TestIrq_Handler() noexcept
{
	rawSink = Helper(4);
}

// Not found: the allocation is before the region and the release after it, the pattern of the fixes
void AllocateBeforeRegion() noexcept
{
	int *const p = new int(3);
	{
		Locker lock;
		sink = p;
	}
	delete p;
}

// Not found: the region is closed on every path before the allocation
void ConditionalRegion(bool b) noexcept
{
	if (b)
	{
		Locker lock;
		selector = 1;
	}
	rawSink = malloc(2);
}

// Not found: interrupts are enabled again before the allocation
void AllocateAfterRestore() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	selector = 3;
	SetPrimask(flags);
	sink = new int(4);
}

// Not found: a switch outside a region
void SwitchOutsideRegion() noexcept
{
	switch (selector)
	{
	case 0: rawSink = Helper(2); break;
	case 1: selector = 4; break;
	case 2: selector = 6; break;
	case 3: selector = 8; break;
	case 4: selector = 10; break;
	default: break;
	}
	Locker lock;
	selector = 12;
}

// Not found: an exception handler that does not allocate
extern "C" void QuietIrq_Handler() noexcept
{
	selector = 0;
}

// Keep the static function in the program
void CallStatic() noexcept
{
	StaticSuspended();
}

// The cases of the unmask check (--unmask), keys in ../expected-unmask.txt. BASEPRI 0xa0 is NvicPriorityStep (5) on a board
// with 3 priority bits, as BasePriorityBooster(NvicPriorityStep) sets it. The functions are not inlined, so that each finding has
// its own name

// Found: a FreeRTOS call with BASEPRI raised, whose critical section ends with BASEPRI 0, as Move::OnEndstopOrZProbeStatesChanged
// wakes the Move task. Not found with RESTORING_PORT, whose critical section restores BASEPRI
__attribute__((noinline)) void BoostedNotify() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	(void)xTaskGenericNotify();
	SetBasepri(old);
}

// Found: a task critical section with BASEPRI raised; xTaskResumeAll ends with a critical section. Not found with RESTORING_PORT
__attribute__((noinline)) void BoostedLocker() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	{
		Locker lock;
		selector = 13;
	}
	SetBasepri(old);
}

// Found: a critical section of the function itself with BASEPRI raised, whose end writes BASEPRI 0. Not found with
// RESTORING_PORT
__attribute__((noinline)) void BoostedCritical() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	vPortEnterCritical();
	selector = 16;
	vPortExitCritical();
	SetBasepri(old);
}

// Found: a call through a function pointer with BASEPRI raised (also a finding of the heap check)
__attribute__((noinline)) void BoostedCallback() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	callback();
	SetBasepri(old);
}

// Found: a task notification with PRIMASK set; vPortEnterCritical ends with cpsie i on the Cortex-M7 r0p1. Not found with
// RESTORING_PORT, whose vPortEnterCritical restores PRIMASK
__attribute__((noinline)) void MaskedNotify() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	(void)xTaskGenericNotify();
	SetPrimask(flags);
}

// Found: a FromISR function with PRIMASK set, which ends its raise of BASEPRI with cpsie i on the Cortex-M7 r0p1. Not found with
// RESTORING_PORT
__attribute__((noinline)) void MaskedGiveFromIsr() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	vTaskGenericNotifyGiveFromISR();
	SetPrimask(flags);
}

// Found: a nested region whose inner end clears PRIMASK for the outer one too
__attribute__((noinline)) void NestedEnable() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	DisableIrq();
	selector = 14;
	EnableIrq();
	selector = 15;
	SetPrimask(flags);
}

// Not found: the notification after the region, as Move::OnEndstopOrZProbeStatesChanged wakes the CAN sender
__attribute__((noinline)) void NotifyAfterBoost() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	const bool wake = (selector != 0);
	SetBasepri(old);
	if (wake)
	{
		(void)xTaskGenericNotify();
	}
}

// Not found: a FreeRTOS call inside a critical section of the function: the nested end keeps the mask
__attribute__((noinline)) void NotifyInCritical() noexcept
{
	vPortEnterCritical();
	(void)xTaskGenericNotify();
	vPortExitCritical();
}

// Not found: a FromISR function with BASEPRI raised; it restores BASEPRI to the value that it found
__attribute__((noinline)) void BoostedGiveFromIsr() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	vTaskGenericNotifyGiveFromISR();
	SetBasepri(old);
}

// Not found: a callee that saves and restores PRIMASK, as IrqSave and IrqRestore do
__attribute__((noinline)) void SaveRestoreHelper() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	selector = 17;
	SetPrimask(flags);
}

__attribute__((noinline)) void MaskedSaveRestore() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	SaveRestoreHelper();
	SetPrimask(flags);
}

// The cases of a call that can block inside a region (target xPortTaskCanBlock), which only the RESTORING_PORT build has. Without
// RESTORING_PORT, the critical section of the take lifts the mask instead, as for BoostedNotify and MaskedNotify

// Found: a take with a timeout with BASEPRI raised (without RESTORING_PORT: -> vPortExitCritical)
__attribute__((noinline)) void BoostedTake() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	(void)xQueueSemaphoreTake(10);
	SetBasepri(old);
}

// Found: a take with a timeout with PRIMASK set (without RESTORING_PORT: -> vPortEnterCritical)
__attribute__((noinline)) void MaskedTake() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	(void)xQueueSemaphoreTake(10);
	SetPrimask(flags);
}

// Found with RESTORING_PORT: a take with a timeout inside a critical section of the function. Not found without it, as
// NotifyInCritical
__attribute__((noinline)) void TakeInCritical() noexcept
{
	vPortEnterCritical();
	(void)xQueueSemaphoreTake(10);
	vPortExitCritical();
}

// Found as well: a take with timeout 0 with BASEPRI raised, which cannot block. The check does not evaluate the argument, so such
// a call needs an allowlist entry with the reason (without RESTORING_PORT: -> vPortExitCritical)
__attribute__((noinline)) void BoostedPoll() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	(void)xQueueSemaphoreTake(0);
	SetBasepri(old);
}

// Not found: the take after the region
__attribute__((noinline)) void TakeAfterBoost() noexcept
{
	const uint32_t old = GetBasepri();
	SetBasepriMax(0xa0);
	selector = 18;
	SetBasepri(old);
	(void)xQueueSemaphoreTake(10);
}

// Functions that the linker places far from the code (section .far, see SELFTEST_FLAGS in ../../Makefile), so that the calls
// to them go through a veneer, as those of Duet3Expansion to its step functions in RAM. Their C++ names make the veneer names
// the mangled ones (___Z9FarHelperj_veneer)
__attribute__((noinline, section(".far"))) void *FarHelper(unsigned int n) noexcept { return malloc(n); }
__attribute__((noinline, section(".far"))) void FarEnable() noexcept { EnableIrq(); }

// Found: an allocation that the region reaches through a veneer
__attribute__((noinline)) void SuspendedVeneer() noexcept
{
	Locker lock;
	rawSink = FarHelper(5);
}

// Found: a clear of PRIMASK that the region reaches through a veneer
__attribute__((noinline)) void MaskedVeneer() noexcept
{
	const uint32_t flags = GetPrimask();
	DisableIrq();
	FarEnable();
	SetPrimask(flags);
}

extern "C" void Reset_Handler() noexcept
{
	SuspendedNew(); LockerMalloc(); OutOfLineFree(nullptr); CallsHelperInCritical(); MaskedNew(); BoostedMalloc();
	SuspendedCallback(); CallStatic(); SwitchInRegion(); AllocateBeforeRegion(); ConditionalRegion(true);
	AllocateAfterRestore(); SwitchOutsideRegion();
	BoostedNotify(); BoostedLocker(); BoostedCritical(); BoostedCallback(); MaskedNotify(); MaskedGiveFromIsr(); NestedEnable();
	NotifyAfterBoost(); NotifyInCritical(); BoostedGiveFromIsr(); MaskedSaveRestore();
	BoostedTake(); MaskedTake(); TakeInCritical(); BoostedPoll(); TakeAfterBoost();
	SuspendedVeneer(); MaskedVeneer();
	for (;;) { }
}

// The vector table: the initial stack pointer, the reset handler, then the exception handlers
extern "C" __attribute__((used, section(".vectors"))) void (*const exception_table[])() noexcept =
{
	nullptr, Reset_Handler, TestIrq_Handler, QuietIrq_Handler
};
