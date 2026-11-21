// Test stand-in for src/RepRapFirmware.h: what the real src/Movement/StepTimer.h and the extracted functions need from
// RepRapFirmware.h, CoreN2G and RRFLibraries, built as for the Duet 3 MB6HC. The processor macros (SAME70, SAME5x, SAM4E, SAM4S)
// stay undefined, so StepTimer.h declares GetTimerTicks() without inlining a register read; the test provides the step clock.

#ifndef ZPROBE_TIMER_TEST_REPRAPFIRMWARE_H
#define ZPROBE_TIMER_TEST_REPRAPFIRMWARE_H

#include <cstddef>
#include <cstdint>
#include <utility>

// eCv annotations (RRFLibraries ecv_original.h)
#define _ecv_array
#define _ecv_from
#define _ecv_null
#define SPEED_CRITICAL
#define THROWS(...)		noexcept(false)						// as src/RepRapFirmware.h:152

#define SUPPORT_CAN_EXPANSION		1
#define SUPPORT_REMOTE_COMMANDS		0
#define SUPPORT_SCANNING_PROBES		1
#define ALLOCATE_DEFAULT_PORTS		0

constexpr size_t MaxZProbes = 4;									// any value > 1 will do here
constexpr size_t MaxZProbeProgramBytes = 8;							// CANlib src/RRF3Common.h:47

// The step clock of all Duet 3 boards, src/RepRapFirmware.h:643
constexpr uint32_t StepClockRate = 48000000/64;						// 750kHz

// CoreN2G src/CoreIO.h:422-434, the constructors used here
union CallbackParameter
{
	void *vp;
	uint32_t u32;
	int32_t i32;

	explicit CallbackParameter(void *pp) noexcept : vp(pp) { }
	explicit CallbackParameter(uint32_t pp) noexcept : u32(pp) { }
	CallbackParameter() noexcept : u32(0) { }
};

// Interrupt priorities. The test runs the step ISR only between the statements of the main task that it plays, so the base
// priority boost of StepTimer::ScheduleCallback and CancelCallback (CoreN2G src/CoreIO.h:394) has nothing to shut out.
typedef uint32_t NvicPriority;
const NvicPriority NvicPriorityStep = 5;							// src/RepRapFirmware.h:760

class BasePriorityBooster
{
public:
	explicit BasePriorityBooster(uint32_t) noexcept { }
	~BasePriorityBooster() { }
};

class AtomicCriticalSectionLocker
{
public:
	AtomicCriticalSectionLocker() noexcept { }
	~AtomicCriticalSectionLocker() { }
};

class StringRef;

// The real DeleteObject, copied out of src/RepRapFirmware.h by the Makefile
#include <DeleteObject.inc>

#endif
