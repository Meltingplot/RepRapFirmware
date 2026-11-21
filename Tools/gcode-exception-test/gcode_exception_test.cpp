// Host test of the GCodeException that Heater::SetTemperature, SwitchEndstop::PrimeAxis and
// CanInterface::EnableRemoteStallEndstop throw with the text of the reply buffer of the call that failed. The real
// GCodeException.cpp is linked in, the three functions are the real text that the Makefile copies out of the sources, the
// classes they belong to are the stand-ins below. See README.md.

#include <RepRapFirmware.h>
#include <GCodes/GCodeException.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>
#include <RemoteInputHandle.h>
#include <cstdio>
#include <string>
#include <type_traits>

#if defined(__SANITIZE_ADDRESS__)
# include <sanitizer/asan_interface.h>
#endif

extern "C" void debugPrintf(const char *_ecv_array fmt, ...) noexcept
{
	va_list vargs;
	va_start(vargs, fmt);
	vprintf(fmt, vargs);
	va_end(vargs);
}

// What the stand-ins for the failing calls write into the reply buffer of the function that throws, the result they return,
// and the buffer itself. If the buffer is a local variable, it is in a stack frame that no longer exists once the exception has
// left that function.
static const char *standInText = "";
static GCodeResult standInResult = GCodeResult::ok;
static std::string replyAtThrow;				// the text of the reply buffer when the function threw
static char *replyBuffer = nullptr;
static size_t replyCapacity = 0;
static bool replyOnStack = false;				// whether the buffer is a local variable of the function that threw

static GCodeResult StandInReply(const StringRef& reply) noexcept
{
	if (standInText[0] != 0)
	{
		reply.lcat(standInText);				// CanInterface::SendRequestAndGetCustomReply appends the reply text with lcatn
	}
	replyAtThrow = reply.c_str();
	replyBuffer = reply.Pointer();
	replyCapacity = reply.Capacity();
#if defined(__SANITIZE_ADDRESS__)
	void *regionAddress;
	size_t regionSize;
	replyOnStack = strcmp(__asan_locate_address(replyBuffer, nullptr, 0, &regionAddress, &regionSize), "stack") == 0;
#endif
	return standInResult;
}

// ---- Stand-ins for Heater::SetTemperature (src/Heating/Heater.cpp) ----

// CANlib RRF3Common.h, same order: SetTemperature switches the heater on if its mode is above 'suspended'
enum class HeaterMode : uint8_t { fault = 0, offline, off, suspended, cooling, stable, heating };
enum class HeaterFunction : uint8_t { none, tool };

class Heat
{
public:
	HeaterFunction GetHeaterFunction(unsigned int) const noexcept { return HeaterFunction::tool; }
};

class RepRap
{
public:
	Heat& GetHeat() noexcept { return heat; }

private:
	Heat heat;
};

static RepRap reprap;

class FopDt
{
public:
	void CalcPidConstants(float) noexcept { }
};

class Heater
{
public:
	void SetTemperature(float t, bool activeNotStandby) THROWS(GCodeException);

private:
	float GetHighestTemperatureLimit() const noexcept { return 300.0; }
	float GetLowestTemperatureLimit() const noexcept { return -273.0; }
	unsigned int GetHeaterNumber() const noexcept { return heaterNumber; }
	HeaterMode GetMode() const noexcept { return HeaterMode::heating; }

	// LocalHeater::SwitchOn prints the reason into the reply, RemoteHeater::SwitchOn gets it from the expansion board
	GCodeResult SwitchOn(const StringRef& reply) noexcept { return StandInReply(reply); }

	FopDt model;
	unsigned int heaterNumber = 1;
	float activeTemperature = 0.0, standbyTemperature = 0.0;
	HeaterFunction function = HeaterFunction::none;
	bool active = true;
};

// ---- Stand-ins for SwitchEndstop::PrimeAxis (src/Endstops/SwitchEndstop.cpp) ----

class Kinematics
{
public:
	LogicalDrivesBitmap GetControllingDrives(size_t axis, bool) const noexcept { return LogicalDrivesBitmap::MakeFromBits(axis); }
};

struct AxisDriversConfig
{
	uint8_t numDrivers = 1;
};

class SwitchEndstop
{
public:
	void PrimeAxis(const Kinematics &_ecv_from kin, const AxisDriversConfig& axisDrivers, float speed) THROWS(GCodeException);

private:
	typedef Bitmap<uint16_t> PortsBitmap;

	unsigned int GetAxis() const noexcept { return 2; }

	CanAddress boardNumbers[1] = { 3 };			// one switch, on expansion board 3
	bool states[1] = { false };
	size_t numPortsUsed = 1;
	PortsBitmap portsLeftToTrigger;
	size_t numPortsLeftToTrigger = 0;
	bool stopAll = false;
	bool haveTriggerTime = false;
};

// ---- Stand-ins for CanInterface::EnableRemoteStallEndstop (src/CAN/CanInterface.cpp) ----

typedef uint16_t CanRequestId;					// CANlib CanId.h

struct DriverId
{
	uint8_t localDriver;
	CanAddress boardAddress;
};

struct CanMessageEnableStallEndstop				// CANlib CanMessageFormats.h, the fields that the function sets
{
	static constexpr uint16_t typeMotorLoad = 0;
	static constexpr uint16_t typeEncoder = 1;

	uint16_t endstopType;
	uint16_t driverNumber;
	float speed;
};

class CanMessageBuffer
{
public:
	static CanMessageBuffer *Allocate() noexcept { return &theBuffer; }

	template<class T> T *SetupRequestMessage(CanRequestId, CanAddress, CanAddress) noexcept
	{
		static_assert(std::is_same_v<T, CanMessageEnableStallEndstop>);
		return &enableStallEndstop;
	}

private:
	static CanMessageBuffer theBuffer;
	CanMessageEnableStallEndstop enableStallEndstop;
};

CanMessageBuffer CanMessageBuffer::theBuffer;

namespace CanInterface
{
	CanAddress GetCanAddress() noexcept { return 0; }
	CanRequestId AllocateRequestId(CanAddress, CanMessageBuffer *) noexcept { return 1; }

	// The real one appends the text of the expansion board's standard reply, or its own timeout message
	GCodeResult SendRequestAndGetStandardReply(CanMessageBuffer *, CanRequestId, const StringRef& reply, uint8_t *_ecv_null) noexcept
	{
		return StandInReply(reply);
	}

	// The real one goes through ChangeInputMonitor and SendRequestAndGetStandardReply
	GCodeResult EnableHandle(CanAddress, RemoteInputHandle, bool, bool *_ecv_null, const StringRef &reply) noexcept
	{
		return StandInReply(reply);
	}

	void EnableRemoteStallEndstop(DriverId did, float speed, bool useEncoder) THROWS(GCodeException);
}

// ---- The real functions ----

#include "Throwers.inc"

// ---- Test cases ----

static unsigned int numPassed = 0, numFailed = 0;

// Plays GCodeMachineState::errorMessage. GCodes::HandleGcode catches the exception of a G0/G1 move (GCodes2.cpp) and
// GCodes::ProbeTool that of M585 (GCodes6.cpp), and both copy it there with GCodeMachineState::SetError.
// GCodeMachineState::RetrieveStateMachineResult formats it later, after the state machine has finished.
static GCodeException errorMessage;

// Calls f, which should throw, and keeps the exception as GCodeMachineState::SetError does
template<class F> static bool __attribute__((noinline)) CallAndKeepError(F f) noexcept
{
	try
	{
		f();
	}
	catch (const GCodeException& exc)
	{
		errorMessage = exc;
		return true;
	}
	return false;
}

// Plays what the main task does before the kept message is formatted: other calls use the stack where the frame of the function
// that threw was. Called from the same function as CallAndKeepError, so its frame covers that frame.
static void __attribute__((noinline)) UseStack() noexcept
{
	volatile char area[65536];
	for (size_t i = 0; i < sizeof(area); ++i)
	{
		area[i] = '#';
	}
}

// Formats the kept message as GCodeMachineState::RetrieveStateMachineResult does. If the reply buffer was a local variable of the
// function that threw, the AddressSanitizer build marks it as unusable meanwhile. Before each throw the compiler calls
// __asan_handle_no_return, which cleans up the shadow memory of the stack (sanitizer/asan_interface.h), so whether ASan sees the
// read of the unwound frame by itself depends on its fake stack (ASAN_OPTIONS detect_stack_use_after_return).
static std::string KeptMessage(const GCodeBuffer *_ecv_null gb) noexcept
{
#if defined(__SANITIZE_ADDRESS__)
	if (replyOnStack)
	{
		__asan_poison_memory_region(replyBuffer, replyCapacity);
	}
#endif
	String<GCodeReplyLength> reply;
	errorMessage.GetMessage(reply.GetRef(), gb);
#if defined(__SANITIZE_ADDRESS__)
	if (replyOnStack)
	{
		__asan_unpoison_memory_region(replyBuffer, replyCapacity);
	}
#endif
	return reply.c_str();
}

static void Report(const char *name, bool thrown, const std::string& message, const std::string& expectedEnd) noexcept
{
	const bool ok = thrown
					&& message.size() >= expectedEnd.size()
					&& message.compare(message.size() - expectedEnd.size(), expectedEnd.size(), expectedEnd) == 0;
	if (ok)
	{
		++numPassed;
		printf("PASS %s: \"%s\"\n", name, message.c_str());
	}
	else
	{
		++numFailed;
		if (!thrown)
		{
			printf("FAIL %s: nothing was thrown\n", name);
		}
		else
		{
			printf("FAIL %s: the message is \"%s\", it should end with the reply \"%s\"\n", name, message.c_str(), expectedEnd.c_str());
		}
	}
}

// Sets the temperature of a heater whose SwitchOn fails with 'text', as M104, M140, G10 and a tool change do
static void __attribute__((noinline)) HeaterCase(const char *name, const char *text) noexcept
{
	standInText = text;
	standInResult = GCodeResult::error;
	Heater heater;
	const bool thrown = CallAndKeepError([&heater]() { heater.SetTemperature(200.0, true); });
	const std::string expected = replyAtThrow;
	UseStack();
	const GCodeBuffer gb('M', 104);
	Report(name, thrown, KeptMessage(&gb), expected);
}

// A G1 H1 move with a switch on an expansion board that fails to enable it with 'text'
static void __attribute__((noinline)) SwitchEndstopCase(const char *name, const char *text) noexcept
{
	standInText = text;
	standInResult = GCodeResult::error;
	SwitchEndstop endstop;
	const bool thrown = CallAndKeepError([&endstop]() { endstop.PrimeAxis(Kinematics(), AxisDriversConfig(), 10.0); });
	const std::string expected = replyAtThrow;
	UseStack();
	const GCodeBuffer gb('G', 1);
	Report(name, thrown, KeptMessage(&gb), expected);
}

// A G1 H1 move with a stall endstop on an expansion board that fails to enable it with 'text'. Then, before the first error is
// reported, the function runs again (another homing move, e.g. in the other motion system) and gets 'secondText' and
// 'secondResult'. The first error must still report its own reply.
static void __attribute__((noinline)) StallEndstopCase(const char *name, const char *text, const char *secondText, GCodeResult secondResult) noexcept
{
	standInText = text;
	standInResult = GCodeResult::error;
	const DriverId did = { 0, 3 };
	const bool thrown = CallAndKeepError([did]() { CanInterface::EnableRemoteStallEndstop(did, 10.0, false); });
	const std::string expected = replyAtThrow;
	char *const firstReplyBuffer = replyBuffer;
	const size_t firstReplyCapacity = replyCapacity;
	const bool firstReplyOnStack = replyOnStack;

	if (secondText != nullptr)
	{
		const GCodeException kept = errorMessage;
		standInText = secondText;
		standInResult = secondResult;
		(void)CallAndKeepError([did]() { CanInterface::EnableRemoteStallEndstop(did, 10.0, false); });
		errorMessage = kept;
		replyBuffer = firstReplyBuffer;
		replyCapacity = firstReplyCapacity;
		replyOnStack = firstReplyOnStack;
	}

	UseStack();
	const GCodeBuffer gb('G', 1);
	Report(name, thrown, KeptMessage(&gb), expected);
}

int main()
{
	// LocalHeater::SwitchOn, src/Heating/LocalHeater.cpp
	HeaterCase("heater: reply of the local heater", "Heater 1 not switched on due to temperature fault");
	// Duet3Expansion LocalHeater::SwitchOn, received by RemoteHeater::SwitchOn: longer than the 50 characters of StringLength50
	HeaterCase("heater: reply of an expansion board, 61 characters", "Failed to turn on heater 1 because it has not been calibrated");
	// CanInterface::SendRequestAndGetCustomReply when the board does not answer (6068 = setHeaterTemperatureV1)
	HeaterCase("heater: CAN timeout, 61 characters", "Timed out awaiting response from board 3 to request type 6068");
	HeaterCase("heater: reply that fills the String<StringLength100>",
				"0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqr");
	// A made-up reply with a percent sign, which must be printed as it is
	HeaterCase("heater: reply with a percent sign", "PWM limit 80%u, heater 1 not switched on");

	// Duet3Expansion InputMonitor::Change, received through CanInterface::EnableHandle
	SwitchEndstopCase("switch endstop: reply of the expansion board", "Board 3 does not have input handle 1080");
	// CanInterface::SendRequestAndGetCustomReply when the board does not answer (6061 = changeInputMonitorV1)
	SwitchEndstopCase("switch endstop: CAN timeout, 61 characters", "Timed out awaiting response from board 3 to request type 6061");
	SwitchEndstopCase("switch endstop: reply with a percent sign", "input 2%d of board 3 not configured");		// made up

	// Duet3Expansion ClosedLoop::EnableStallEndstop: 102 characters, of which the reply buffer keeps 100
	StallEndstopCase("stall endstop: reply of 100 characters",
				"driver 3.0 has no position warning threshold configured, see the first value of the M569.1 E parameter",
				nullptr, GCodeResult::ok);
	// Duet3Expansion SmartDrivers::SetStallEndstopReporting (TMC51xx.cpp) and Move::SetStallEndstopReporting
	StallEndstopCase("stall endstop: the function succeeds again before the error is reported",
				"driver 3.0 is not in spreadCycle mode", "", GCodeResult::ok);
	StallEndstopCase("stall endstop: the function fails again before the error is reported",
				"driver 3.0 is not in spreadCycle mode", "board 3 has no driver 7", GCodeResult::error);
	StallEndstopCase("stall endstop: reply with a percent sign", "driver 3.0 stalled at 5%u of its speed", nullptr, GCodeResult::ok);	// made up

	printf("%u passed, %u failed\n", numPassed, numFailed);
	return (numFailed == 0) ? 0 : 1;
}
