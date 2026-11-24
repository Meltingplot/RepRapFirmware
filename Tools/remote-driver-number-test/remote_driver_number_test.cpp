// Host test of the driver numbers in two CAN requests that RRF handles in expansion mode (M954): the real text of
// Move::EutHandleSetDriverStates and Move::StopDriversFromRemote (src/Movement/Move2.cpp), which the Makefile extracts, with the sizes
// of the Duet 3 Mini 5+ (src/Config/Pins_Duet3Mini.h), whose 16-bit driver bitmap reaches beyond its arrays of MaxAxesPlusExtruders
// entries. See README.md.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <General/Bitmap.h>

#include "Sizes.inc"											// NumDirectDrivers and MaxAxesPlusExtruders of the Mini 5+

enum class GCodeResult : uint8_t { ok, error };

// Just what the extracted functions use of StringRef (RRFLibraries General/StringRef.h)
class StringRef
{
public:
	explicit StringRef(std::string& s) noexcept : str(s) { }
	void copy(const char *s) const noexcept { str = s; }
	void lcatf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		char buf[100];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (!str.empty()) { str += '\n'; }
		str += buf;
	}

private:
	std::string& str;
};

namespace CanInterface
{
	inline unsigned int GetCanAddress() noexcept { return 121; }
}

// As in CANlib src/CanMessageFormats.h (DriverStateControl, CanMessageMultipleDrivesRequest) and src/Duet3Common.h
constexpr unsigned int MaxLinearDriversPerCanSlave = 8;

struct __attribute__((packed)) DriverStateControl
{
	uint16_t mode : 2,
			 zero : 6,
			 idlePercent : 8;

	static constexpr uint16_t driverDisabled = 0, driverIdle = 1, driverActive = 2;
};

template<class T> struct __attribute__((packed)) CanMessageMultipleDrivesRequest
{
	uint16_t requestId : 12,
			 zero : 4;
	uint16_t driversToUpdate;
	T values[MaxLinearDriversPerCanSlave];

	constexpr size_t GetActualDataLength() const volatile noexcept { return sizeof(uint16_t) * 2 + __builtin_popcount(driversToUpdate) * sizeof(T); }
};

// The members of Move (src/Movement/Move.h) that the extracted functions use. The two arrays are allocated with exactly
// MaxAxesPlusExtruders entries, so that AddressSanitizer sees an access beyond them; the driver functions record their calls
class Move
{
public:
	enum class DriverStatus : uint8_t { disabled, idle, enabled };

	Move() noexcept
		: driverState(new DriverStatus[MaxAxesPlusExtruders]()), motorCurrents(new float[MaxAxesPlusExtruders]())
	{
		for (size_t i = 0; i < MaxAxesPlusExtruders; ++i) { motorCurrents[i] = 1000.0f + i; }
	}

	GCodeResult EutHandleSetDriverStates(const CanMessageMultipleDrivesRequest<DriverStateControl>& msg, size_t dataLength, const StringRef& reply) noexcept;
	void StopDriversFromRemote(uint16_t whichDrives) noexcept;

	std::unique_ptr<DriverStatus[]> driverState;
	std::unique_ptr<float[]> motorCurrents;
	std::vector<size_t> enabled, disabled, currents, stopped;

private:
	void EnableOneLocalDriver(size_t driver, float) noexcept { enabled.push_back(driver); }
	void DisableOneLocalDriver(size_t driver) noexcept { disabled.push_back(driver); }
	void UpdateMotorCurrent(size_t driver, float) noexcept { currents.push_back(driver); }
	void StopDriveFromRemote(size_t drive) noexcept { stopped.push_back(drive); }
};

typedef Bitmap<uint16_t> LocalDriversBitmap;					// as in src/RepRapFirmware.h

#include "Move2.inc"

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, const std::string& why) noexcept
{
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s:%s\n", name, why.c_str());
	}
}

static std::string Highest(const std::vector<size_t>& drivers, const char *what) noexcept
{
	for (size_t d : drivers)
	{
		if (d >= NumDirectDrivers) { return std::string(" ") + what + " driver " + std::to_string(d); }
	}
	return "";
}

// A set-driver-states request in a buffer of 64 bytes, the size of a CAN FD message, as CommandProcessor passes it
static GCodeResult SetDriverStates(Move& move, uint16_t drivers, uint16_t mode, std::string& reply) noexcept
{
	alignas(4) uint8_t buffer[64] = { };
	auto& msg = *reinterpret_cast<CanMessageMultipleDrivesRequest<DriverStateControl> *>(buffer);
	msg.driversToUpdate = drivers;
	const unsigned int count = __builtin_popcount(drivers);
	DriverStateControl *values = reinterpret_cast<DriverStateControl *>(buffer + 2 * sizeof(uint16_t));
	for (unsigned int i = 0; i < count; ++i)
	{
		values[i].mode = mode;
		values[i].idlePercent = 30;
	}
	StringRef r(reply);
	return move.EutHandleSetDriverStates(msg, msg.GetActualDataLength(), r);
}

int main()
{
	printf("NumDirectDrivers = %zu, MaxAxesPlusExtruders = %zu (Duet 3 Mini 5+)\n", NumDirectDrivers, MaxAxesPlusExtruders);
	const uint16_t allLocal = (uint16_t)((1u << NumDirectDrivers) - 1);

	{
		Move move;
		std::string reply, why;
		if (SetDriverStates(move, allLocal, DriverStateControl::driverActive, reply) != GCodeResult::ok) { why += " the result was not ok"; }
		if (!reply.empty()) { why += " reply \"" + reply + "\""; }
		if (move.enabled.size() != NumDirectDrivers) { why += " " + std::to_string(move.enabled.size()) + " drivers were enabled"; }
		for (size_t d = 0; d < NumDirectDrivers; ++d)
		{
			if (move.driverState[d] != Move::DriverStatus::enabled) { why += " driver " + std::to_string(d) + " is not enabled"; }
		}
		Report("set-driver-states for every local driver enables each of them", why);
	}

	for (uint16_t mode : { DriverStateControl::driverActive, DriverStateControl::driverIdle, DriverStateControl::driverDisabled })
	{
		Move move;
		std::string reply, why;
		if (SetDriverStates(move, 0xFFFF, mode, reply) != GCodeResult::error) { why += " the result was not error"; }
		for (unsigned int d = NumDirectDrivers; d < 16; ++d)
		{
			if (reply.find("No such driver 121." + std::to_string(d)) == std::string::npos)
			{
				why += " no \"No such driver\" for driver " + std::to_string(d);
			}
		}
		why += Highest(move.enabled, "enabled") + Highest(move.disabled, "disabled") + Highest(move.currents, "current set for");
		const size_t handled = move.enabled.size() + move.disabled.size() + move.currents.size();
		if (handled != NumDirectDrivers) { why += " " + std::to_string(handled) + " drivers were handled"; }
		const std::string name = std::string("set-driver-states (mode ") + std::to_string(mode)
								+ ") for drivers 0 to 15 handles the local drivers only and reports the others";
		Report(name.c_str(), why);
	}

	{
		Move move;
		std::string reply, why;
		if (SetDriverStates(move, 1u << 12, DriverStateControl::driverActive, reply) != GCodeResult::error) { why += " the result was not error"; }
		if (!move.enabled.empty()) { why += " a driver was enabled"; }
		Report("set-driver-states for driver 12 only, the first one beyond driverState, is refused", why);
	}

	{
		Move move;
		std::string why;
		move.StopDriversFromRemote(0xFFFF);
		why += Highest(move.stopped, "stopped");
		if (move.stopped.size() != NumDirectDrivers) { why += " " + std::to_string(move.stopped.size()) + " drivers were stopped"; }
		Report("stop-drivers for drivers 0 to 15 stops the local drivers only", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
