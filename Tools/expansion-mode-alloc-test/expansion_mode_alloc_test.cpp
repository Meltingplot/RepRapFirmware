// Host test of M954 (CanInterface::SwitchToExpansionMode in src/CAN/CanInterface.cpp), compiled from the real text of
// CanInterface::SwitchToExpansionMode, GCodes::SwitchToExpansionMode (src/GCodes/GCodes3.cpp), FilamentMonitor::DeleteAll
// (src/FilamentMonitors/FilamentMonitor.cpp) and DeleteObject (src/RepRapFirmware.h) that the Makefile extracts.
//
// The switch to expansion mode deletes all filament monitors. FilamentMonitor::DeleteAll takes a WriteLocker on
// filamentMonitorsLock, which allocates a lock record (RRFLibraries ReadWriteLock::LockForWriting) and may wait for a reader, and it
// frees each monitor with delete, so with RRF's free. RRF's malloc and free take the malloc mutex only while the scheduler is running
// (GetMallocMutex in src/Platform/Tasks.cpp), and a task must not wait for a lock while the scheduler is suspended. M954 runs in the
// main task (GCodes2.cpp), and the CAN receiver task switches when a main board asks it to enter test mode (CommandProcessor.cpp).
// Up to 3.7-dev de1bfe598, CanInterface::SwitchToExpansionMode called GCodes::SwitchToExpansionMode, and so DeleteAll, inside its
// TaskCriticalSectionLocker, which suspends the scheduler.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter, the WriteLocker, filament monitors and the
// rest with stand-ins. The write lock must be taken, and each monitor freed, with the scheduler running. It also checks what the switch
// does: every filament monitor is deleted and its pointer cleared, the address, expansion mode and test mode are set, the axes are
// mapped to the local drivers, the move system is switched and the CAN filters are reset with the new address.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from

typedef uint8_t CanAddress;									// CoreN2G src/CoreTypes.h

constexpr size_t NumDirectDrivers = 6;						// as on the MB6HC
constexpr size_t MaxAxes = 15;

static unsigned int suspended = 0;							// uxSchedulerSuspended
static std::vector<std::string> failures;
static unsigned int writeLocks = 0, monitorsFreed = 0, filterResets = 0;
static CanAddress addressAtFilterReset = 0;

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

// Just what DeleteAll uses of ReadWriteLock and WriteLocker (RRFLibraries RTOSIface.h). LockForWriting allocates a lock record and
// waits for the readers to finish, so it must run with the scheduler running.
class ReadWriteLock { };

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock&) noexcept
	{
		if (suspended != 0)
		{
			failures.push_back("the filament monitor write lock was taken with the scheduler suspended");
		}
		++writeLocks;
	}
};

// Just what DeleteAll uses of FilamentMonitor (src/FilamentMonitors/FilamentMonitor.h). Its operator delete stands for RRF's free.
class FilamentMonitor
{
public:
	virtual ~FilamentMonitor() noexcept { }

	static void operator delete(void *p) noexcept
	{
		if (suspended != 0)
		{
			failures.push_back("a filament monitor was freed with the scheduler suspended");
		}
		++monitorsFreed;
		::operator delete(p);
	}

	static void DeleteAll() noexcept;

	static constexpr size_t NumFilamentMonitors = 4;
	static ReadWriteLock filamentMonitorsLock;
	static FilamentMonitor *_ecv_from _ecv_null filamentSensors[NumFilamentMonitors];
};

ReadWriteLock FilamentMonitor::filamentMonitorsLock;
FilamentMonitor *_ecv_from _ecv_null FilamentMonitor::filamentSensors[NumFilamentMonitors] = { };

// Just what the extracted functions use of DriverId, Move, GCodes and RepRap
struct DriverId
{
	uint8_t localDriver = 0xFF;
	void SetLocal(size_t d) noexcept { localDriver = (uint8_t)d; }
};

class Move
{
public:
	void SwitchToExpansionMode() noexcept { inExpansionMode = true; }		// as in src/Movement/Move.h
	void SetAxisDriversConfig(size_t axis, size_t numValues, const DriverId driverNumbers[]) noexcept
	{
		if (numValues == 1 && axis < MaxAxes) { axisDriver[axis] = driverNumbers[0].localDriver; }
	}

	bool inExpansionMode = false;
	uint8_t axisDriver[MaxAxes] = { };
};

class GCodes
{
public:
	void SwitchToExpansionMode() noexcept;

	static constexpr const char *_ecv_array AllowedAxisLetters = "XYZUVWABCDabcdefghijklmnopqrstuvwxyz";
	size_t numExtruders = 2, numVisibleAxes = 3, numTotalAxes = 3;
	char axisLetters[MaxAxes + 1] = "XYZ";
	bool isRemotePrinting = true;
};

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return gcodes; }
	Move& GetMove() noexcept { return move; }

	GCodes gcodes;
	Move move;
};

static RepRap reprap;

// The CanInterface state in src/CAN/CanInterface.cpp that SwitchToExpansionMode sets, and ReInit, which resets the CAN filters
static CanAddress myAddress = 0;
static bool inExpansionMode = false;
static bool inTestMode = false;

static void ReInit() noexcept
{
	++filterResets;
	addressAtFilterReset = myAddress;
}

namespace CanInterface
{
	void SwitchToExpansionMode(CanAddress addr, bool useTestMode) noexcept;
}

#include "ExpansionMode.inc"

static unsigned int passed = 0, failed = 0;

static void Case(const char *name, CanAddress addr, bool useTestMode, unsigned int numMonitors) noexcept
{
	reprap = RepRap();
	myAddress = 0;
	inExpansionMode = inTestMode = false;
	failures.clear();
	writeLocks = monitorsFreed = filterResets = 0;
	for (size_t i = 0; i < numMonitors; ++i)
	{
		FilamentMonitor::filamentSensors[i] = new FilamentMonitor;
	}

	CanInterface::SwitchToExpansionMode(addr, useTestMode);

	std::string why;
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	if (suspended != 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("the scheduler was left suspended");
	}
	if (monitorsFreed != numMonitors
		|| std::any_of(std::begin(FilamentMonitor::filamentSensors), std::end(FilamentMonitor::filamentSensors), [](const FilamentMonitor *fm) { return fm != nullptr; }))
	{
		why += (why.empty() ? "" : "; ") + std::string("not all filament monitors were deleted");
	}
	if (writeLocks == 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("the filament monitor list was not write locked");
	}
	if (myAddress != addr || !inExpansionMode || inTestMode != useTestMode || !reprap.move.inExpansionMode)
	{
		why += (why.empty() ? "" : "; ") + std::string("the address or the modes were not set");
	}
	if (filterResets != 1 || addressAtFilterReset != addr)
	{
		why += (why.empty() ? "" : "; ") + std::string("the CAN filters were not reset once with the new address");
	}
	const GCodes& g = reprap.gcodes;
	bool axesOk = g.numExtruders == 0 && g.numVisibleAxes == NumDirectDrivers && g.numTotalAxes == NumDirectDrivers && !g.isRemotePrinting
					&& strncmp(g.axisLetters, GCodes::AllowedAxisLetters, MaxAxes) == 0;
	for (size_t axis = 0; axis < NumDirectDrivers; ++axis)
	{
		axesOk = axesOk && reprap.move.axisDriver[axis] == axis;
	}
	if (!axesOk)
	{
		why += (why.empty() ? "" : "; ") + std::string("the axes were not mapped to the local drivers");
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
}

int main()
{
	Case("M954 A21 deletes the filament monitors with the scheduler running and switches to expansion mode", 21, false, 3);
	Case("the switch to test mode without filament monitors", 100, true, 0);
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
