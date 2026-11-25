// Host test of the driver data accessors of ExpansionManager (src/CAN/ExpansionManager.cpp), compiled from the real function
// text that the Makefile extracts.
//
// ProcessAnnouncement runs in the CAN receiver task (TaskPriority::CanReceiverPriority). When an expansion board announces
// itself with a different board type or firmware version, e.g. after M997 B updated it, it deletes the board's driverData array
// and allocates a new one, under a write lock on boardsLock. The main task (TaskPriority::SpinPriority) uses the same array in
// StoreDriverDirection and StoreDriverMode for M569 on a remote driver, and in GetDriverDirection. The receiver task has the
// higher priority, so it can run at any instruction of these functions. It can only replace the array while it gets the write
// lock, so an accessor must hold a read lock on boardsLock for as long as it uses the array. The test plays the receiver task
// at the moment the accessor uses an element of the array, and fails if it could take the write lock then.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from

// Just what the accessors use
class ReadWriteLock
{
public:
	void LockForReading() noexcept { ++readers; }
	void ReleaseReader() noexcept { --readers; }
	bool ConditionalLockForWriting() const noexcept { return readers == 0; }		// what the receiver task would get, without keeping it

	int readers = 0;
};

class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock& p_lock) noexcept : lock(p_lock) { lock.LockForReading(); }
	~ReadLocker() { lock.ReleaseReader(); }

	ReadLocker(const ReadLocker&) = delete;
	ReadLocker& operator=(const ReadLocker&) = delete;

private:
	ReadWriteLock& lock;
};

enum class DriverMode : unsigned int { spreadCycle = 2, stealthChop = 3, unknown = 6 };		// the values used here, from src/Movement/StepperDrivers/DriverMode.h

struct DriverId																	// as in src/RepRapFirmware.h
{
	DriverId(uint8_t addr, uint8_t drv) noexcept : localDriver(drv), boardAddress(addr) { }

	uint8_t localDriver;
	uint8_t boardAddress;
};

static void ReceiverTaskRuns(const char *access) noexcept;

// As in src/Movement/StepperDrivers/DriverData.h, plus the moment at which the receiver task runs
class DriverData
{
public:
	bool GetDirection() const noexcept { ReceiverTaskRuns("GetDirection"); return configuredDirection; }
	DriverMode GetMode() const noexcept { ReceiverTaskRuns("GetMode"); return (DriverMode)configuredMode; }
	void StoreDirection(bool dir) noexcept { ReceiverTaskRuns("StoreDirection"); configuredDirection = dir; }
	void StoreMode(uint32_t mode) noexcept { ReceiverTaskRuns("StoreMode"); configuredMode = (uint16_t)mode; }

	// For the checks of the test, without the receiver task
	bool StoredDirection() const noexcept { return configuredDirection; }
	DriverMode StoredMode() const noexcept { return (DriverMode)configuredMode; }

private:
	uint16_t configuredMode = (uint16_t)DriverMode::spreadCycle;
	bool configuredDirection = true;
};

// The members of ExpansionBoardData in src/CAN/ExpansionManager.h that the accessors use
struct ExpansionBoardData
{
	bool HasDriver(size_t driver) const noexcept { return driver < numDrivers && driverData != nullptr; }

	DriverData *_ecv_array _ecv_null driverData = nullptr;
	uint8_t numDrivers = 0;
};

class ExpansionManager
{
public:
	void StoreDriverDirection(DriverId did, bool direction) noexcept;
	void StoreDriverMode(DriverId did, uint32_t mode) noexcept;
	bool GetDriverDirection(DriverId did) const noexcept;
	DriverMode GetDriverMode(DriverId did) const noexcept;

	static ReadWriteLock boardsLock;
	ExpansionBoardData boards[4];
};

ReadWriteLock ExpansionManager::boardsLock;

#include "Accessors.inc"

static std::vector<std::string> failures;

static void ReceiverTaskRuns(const char *access) noexcept
{
	if (ExpansionManager::boardsLock.ConditionalLockForWriting())
	{
		failures.push_back(std::string("the CAN receiver task could replace driverData during DriverData::") + access);
	}
}

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, bool ok, const std::string& why) noexcept
{
	if (ok)
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

// Run one access to board 1, driver 1, and check that the receiver task could not replace the array meanwhile and that the
// lock is free again afterwards
template<class F> static void Case(const char *name, F access) noexcept
{
	ExpansionManager em;
	DriverData drivers[2];
	em.boards[1].driverData = drivers;
	em.boards[1].numDrivers = 2;
	failures.clear();
	std::string why = access(em);
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	if (ExpansionManager::boardsLock.readers != 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("boardsLock is still locked afterwards");
		ExpansionManager::boardsLock.readers = 0;
	}
	Report(name, why.empty(), why);
}

int main()
{
	const DriverId driver(1, 1);

	Case("M569 S stores the direction of a remote driver", [&](ExpansionManager& em) {
		em.StoreDriverDirection(driver, false);
		return (em.boards[1].driverData[1].StoredDirection()) ? std::string("direction not stored") : std::string();
	});
	Case("M569 D stores the mode of a remote driver", [&](ExpansionManager& em) {
		em.StoreDriverMode(driver, (uint32_t)DriverMode::stealthChop);
		return (em.boards[1].driverData[1].StoredMode() != DriverMode::stealthChop) ? std::string("mode not stored") : std::string();
	});
	Case("the direction of a remote driver is read", [&](ExpansionManager& em) {
		return (em.GetDriverDirection(driver)) ? std::string() : std::string("wrong direction");
	});
	Case("the mode of a remote driver is read", [&](ExpansionManager& em) {
		return (em.GetDriverMode(driver) != DriverMode::spreadCycle) ? std::string("wrong mode") : std::string();
	});

	// A board that has not announced any drivers: nothing to use, and the defaults of the accessors
	Case("a board without drivers is left alone", [&](ExpansionManager& em) {
		const DriverId other(2, 0);
		em.StoreDriverDirection(other, false);
		em.StoreDriverMode(other, (uint32_t)DriverMode::stealthChop);
		if (!em.GetDriverDirection(other)) { return std::string("direction is not forwards"); }
		if (em.GetDriverMode(other) != DriverMode::unknown) { return std::string("mode is not unknown"); }
		return std::string();
	});

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
