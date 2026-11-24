// Host test of the driver data that RRF keeps for each expansion board: the real text of the four accessors in
// src/CAN/ExpansionManager.cpp and of the HasDriver(s) members of ExpansionBoardData (src/CAN/ExpansionManager.h), which the Makefile
// extracts. ProcessAnnouncement allocates driverData with the number of drivers that the board announced; the accessors are called with
// the driver number of a G-code (M569, M584 through Move::GetDirectionValue). A driver number at or beyond that count must be treated
// like a board without driver data. See README.md.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// As in src/Movement/StepperDrivers/DriverMode.h (the values that the test uses)
enum class DriverMode : unsigned int { constantOffTime = 0, randomOffTime = 1, spreadCycle = 2, stealthChop = 3, unknown = 6 };

// The members of DriverData (src/Movement/StepperDrivers/DriverData.h) that the accessors use, with its defaults
class DriverData
{
public:
	DriverData() noexcept { configuredDirection = 1; configuredMode = (uint16_t)DriverMode::spreadCycle; }

	bool GetDirection() const noexcept { return configuredDirection; }
	DriverMode GetMode() const noexcept { return (DriverMode)configuredMode; }
	void StoreDirection(bool dir) noexcept { configuredDirection = dir; }
	void StoreMode(uint32_t mode) noexcept { configuredMode = (uint16_t)mode; }

private:
	uint16_t configuredMode : 3,
			 configuredDirection : 1;
};

// The lock of RRFLibraries RTOSIface.h, without an RTOS
class ReadWriteLock { };
class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock&) noexcept { }
};

// As in src/RepRapFirmware.h with SUPPORT_CAN_EXPANSION
struct DriverId
{
	uint8_t localDriver;
	uint8_t boardAddress;
};

// The members of ExpansionBoardData (src/CAN/ExpansionManager.h) that the accessors use
struct ExpansionBoardData
{
#include "BoardMembers.inc"											// HasDrivers() and, with the fix, HasDriver(size_t)

	DriverData *driverData = nullptr;									// an array numDrivers long of objects, or nullptr if numDrivers is zero
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

static unsigned int passed = 0, failed = 0;

static void Report(const std::string& name, bool ok, const std::string& why = "") noexcept
{
	if (ok)
	{
		++passed;
		printf("PASS %s\n", name.c_str());
	}
	else
	{
		++failed;
		printf("FAIL %s%s\n", name.c_str(), why.c_str());
	}
}

int main()
{
	constexpr uint8_t Board = 1, NumDrivers = 3;
	ExpansionManager em;

	// Board 1 announced 3 drivers. The AddressSanitizer build allocates exactly 3, so that it reports an access beyond them. The plain
	// build allocates 8 more with other values behind them: an access beyond the 3 then reads or changes one of those
#ifdef __SANITIZE_ADDRESS__
	constexpr size_t Allocated = NumDrivers;
#else
	constexpr size_t Allocated = NumDrivers + 8;
#endif
	DriverData *const data = new DriverData[Allocated];
	for (size_t i = NumDrivers; i < Allocated; ++i)
	{
		data[i].StoreDirection(false);
		data[i].StoreMode((uint32_t)DriverMode::stealthChop);
	}
	em.boards[Board].driverData = data;
	em.boards[Board].numDrivers = NumDrivers;

	// The drivers that the board has
	em.StoreDriverDirection(DriverId{ 2, Board }, false);
	Report("direction of driver 1.2 is stored", em.GetDriverDirection(DriverId{ 2, Board }) == false);
	Report("direction of driver 1.0 keeps its default", em.GetDriverDirection(DriverId{ 0, Board }) == true);
	em.StoreDriverMode(DriverId{ 0, Board }, (uint32_t)DriverMode::stealthChop);
	Report("mode of driver 1.0 is stored", em.GetDriverMode(DriverId{ 0, Board }) == DriverMode::stealthChop);
	Report("mode of driver 1.1 keeps its default", em.GetDriverMode(DriverId{ 1, Board }) == DriverMode::spreadCycle);

	// A board without driver data, as before
	Report("direction on a board without drivers is forwards", em.GetDriverDirection(DriverId{ 0, 2 }) == true);
	Report("mode on a board without drivers is unknown", em.GetDriverMode(DriverId{ 0, 2 }) == DriverMode::unknown);

	// Driver numbers that the board did not announce: like a board without driver data
	Report("direction of driver 1.3 (the count) is forwards", em.GetDriverDirection(DriverId{ NumDrivers, Board }) == true);
	Report("mode of driver 1.5 is unknown", em.GetDriverMode(DriverId{ 5, Board }) == DriverMode::unknown);
	em.StoreDriverDirection(DriverId{ 4, Board }, true);
	em.StoreDriverMode(DriverId{ 7, Board }, (uint32_t)DriverMode::spreadCycle);
	bool untouched = true;
	for (size_t i = NumDrivers; i < Allocated; ++i)
	{
		untouched = untouched && data[i].GetDirection() == false && data[i].GetMode() == DriverMode::stealthChop;
	}
	Report("storing to drivers 1.4 and 1.7 changes nothing behind the 3 drivers", untouched);
	Report("storing to drivers 1.4 and 1.7 leaves the 3 drivers as they were",
			em.GetDriverDirection(DriverId{ 2, Board }) == false && em.GetDriverMode(DriverId{ 0, Board }) == DriverMode::stealthChop);

	delete[] data;
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
