// Host test of the timeout checks that compare a clock reading with a time stamp that another task or an ISR writes:
// RemoteHeater::GetTemperature, GetAveragePWM and GetMode, RemoteFan::Check, LocalFan::GetRPM, FilamentMonitor::FilamentPresenceKnown,
// IsFilamentPresent and IsMotionDetected, and PulsedFilamentMonitor::Check and IsLocalMotionDetected. The functions are the real text, extracted by the Makefile; the classes around them are
// stand-ins with the members that the functions use. See README.md.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>

#define SUPPORT_CAN_EXPANSION	1

// ---------------------------------------------------------------------------------------------------------------------------------
// The clocks. Each one returns the time that it read, then runs the writer that the test has set up, if any, one tick later: the
// writer has preempted the caller right after the clock read, which is the window that the fix closes.

static uint32_t millisNow, ticksNow;
static std::function<void()> writerAfterClockRead;

static void RunWriter() noexcept
{
	if (writerAfterClockRead)
	{
		const std::function<void()> writer = writerAfterClockRead;
		writerAfterClockRead = nullptr;
		writer();
	}
}

uint32_t millis() noexcept
{
	const uint32_t now = millisNow;
	++millisNow;
	RunWriter();
	return now;
}

namespace StepTimer
{
	uint32_t GetTimerTicks() noexcept
	{
		const uint32_t now = ticksNow;
		ticksNow += 10;
		RunWriter();
		return now;
	}
}

constexpr uint32_t StepClockRate = 48000000/64;				// src/RepRapFirmware.h:642, SAME70 and SAME5x

// ---------------------------------------------------------------------------------------------------------------------------------
// Stand-ins for the types and the classes. The constants are those of the headers named.

// CANlib src/RRF3Common.h:282-304
enum class HeaterMode : uint8_t
{
	fault = 0, offline, off, suspended, cooling, stable, heating, tuning0_settling, tuning0a_calibrating_heater, tuning1_heating_up,
	tuning2_heater_off, tuning3_heater_on
};

struct TemperatureError
{
	enum Value { ok, unknownError };
	TemperatureError(Value) noexcept { }
};

constexpr float SensorTemperature = 21.5;					// what the local sensor reading gives when the remote status is too old

struct HeatStandIn
{
	float GetSensorTemperature(unsigned int, TemperatureError&) const noexcept { return SensorTemperature; }
};

struct RepRapStandIn
{
	HeatStandIn heat;
	const HeatStandIn& GetHeat() const noexcept { return heat; }
} reprap;

class RemoteHeater
{
public:
	float GetTemperature() const noexcept;
	float GetAveragePWM() const noexcept;
	HeaterMode GetMode() const noexcept;
	unsigned int GetSensorNumber() const noexcept { return 0; }

	// src/Heating/RemoteHeater.h:57-71
	enum class TuningState : uint8_t { notTuning = 0, stabilising };
	static constexpr uint32_t RemoteStatusTimeout = 2000;

	HeaterMode lastMode = HeaterMode::off;
	uint8_t averagePwm = 0;
	TuningState tuningState = TuningState::notTuning;
	float lastTemperature = 0.0;
	uint32_t whenLastStatusReceived = 0;
};

struct SensorsBitmap
{
	bool nonEmpty = false;
	bool IsNonEmpty() const noexcept { return nonEmpty; }
};

class RemoteFan
{
public:
	bool Check(bool checkSensors) noexcept;

	static constexpr uint32_t FanReportTimeout = 2000;		// src/Fans/Fan.h:67
	SensorsBitmap sensorsMonitored;
	int32_t lastRpm = -1;
	float lastPwm = -1.0;
	uint32_t whenLastReportReceived = 0;
};

struct IoPort
{
	bool valid = true;
	bool IsValid() const noexcept { return valid; }
};

class LocalFan
{
public:
	int32_t GetRPM() const noexcept;

	IoPort tachoPort;
	uint32_t tachoMultiplier = 0;
	volatile uint32_t tachoLastResetTime = 0;
	volatile uint32_t tachoInterval = 0;
};

class FilamentMonitor
{
public:
	bool FilamentPresenceKnown() const noexcept;
	bool IsFilamentPresent() const noexcept;
	bool IsMotionDetected() const noexcept;
	bool GetLocalFilamentPresent(bool& present) const noexcept { return false; }
	bool IsLocalMotionDetected() const noexcept { return false; }

	static constexpr uint32_t RemoteLiveBitsTimeout = 5000;	// src/FilamentMonitors/FilamentMonitor.h:218
	uint32_t whenRemoteLiveBitsReceived = 0;
	bool hasRemote = true;
	bool remotePresenceValid = false;
	bool remotePresence = false;
	bool remoteMotion = false;
};

// CANlib src/RRF3Common.h:139-147 and :55
enum class FilamentSensorStatus : uint8_t { noMonitor, ok, noDataReceived, noFilament, tooLittleMovement, tooMuchMovement, sensorError };
constexpr uint32_t FilamentMonitorMotionLatchTime = 500;

class PulsedFilamentMonitor
{
public:
	bool Interrupt() noexcept;
	FilamentSensorStatus Check(bool isPrinting, bool fromIsr, uint32_t isrMillis, float filamentConsumed) noexcept;
	bool IsLocalMotionDetected() const noexcept;

	// Check calls these; the test only records whether the "sync overdue" comparison ran
	void Poll() noexcept { }
	FilamentSensorStatus CheckFilament(float amountCommanded, float amountMeasured, bool overdue) noexcept
	{
		if (overdue)
		{
			++overdueComparisons;
		}
		return FilamentSensorStatus::ok;
	}
	uint8_t GetEnableMode() const noexcept { return 1; }

	unsigned int overdueComparisons = 0;

	// src/FilamentMonitors/PulsedFilamentMonitor.h:53-78
	float minimumExtrusionCheckLength = 5.0;
	uint32_t sensorValue = 0;
	uint32_t lastIsrTime = 0;
	uint32_t lastMeasurementTime = 0;
	float extrusionCommandedAtInterrupt = 0.0;
	float extrusionCommandedSinceLastSync = 0.0;
	float movementMeasuredSinceLastSync = 0.0;
	bool wasPrintingAtInterrupt = false;
	bool haveInterruptData = false;
	float extrusionCommandedThisSegment = 0.0;
	float movementMeasuredThisSegment = 0.0;
	uint8_t samplesReceived = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// The real code

#include "RemoteHeater.inc"
#include "RemoteFan.inc"
#include "LocalFan.inc"
#include "FilamentMonitor.inc"
#include "PulsedFilamentMonitor.inc"
#include "PulsedFilamentMonitorH.inc"

// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int runs = 0, failures = 0;

static void Expect(bool ok, const std::string& name, const std::string& what) noexcept
{
	++runs;
	std::printf("%s %s: %s\n", (ok) ? "PASS" : "FAIL", name.c_str(), what.c_str());
	if (!ok)
	{
		++failures;
	}
}

// How the reports arrive: one some time ago and nothing since (fresh or too old), or one some time ago and another one from the writer
// that runs right after the clock read
enum class Arrival { fresh, tooOld, newerDuringCheck };

static const char *ArrivalName(Arrival a) noexcept
{
	return (a == Arrival::fresh) ? "fresh report" : (a == Arrival::tooOld) ? "report too old" : "newer report during the check";
}

static void HeaterCases() noexcept
{
	for (Arrival a : { Arrival::fresh, Arrival::tooOld, Arrival::newerDuringCheck })
	{
		for (int fn = 0; fn < 3; ++fn)
		{
			RemoteHeater h;
			millisNow = 1000000;
			h.lastMode = HeaterMode::heating;
			h.averagePwm = 128;
			h.lastTemperature = 205.0;
			h.whenLastStatusReceived = (a == Arrival::tooOld) ? millisNow - 3000 : millisNow - 250;
			if (a == Arrival::newerDuringCheck)
			{
				// RemoteHeater::UpdateRemoteStatus (src/Heating/RemoteHeater.cpp), in the CanReceiver task
				writerAfterClockRead = [&h]() { h.lastMode = HeaterMode::heating; h.averagePwm = 128; h.lastTemperature = 205.0; h.whenLastStatusReceived = millis(); };
			}
			const bool online = (a != Arrival::tooOld);
			const std::string name = std::string("RemoteHeater::") + ((fn == 0) ? "GetMode" : (fn == 1) ? "GetTemperature" : "GetAveragePWM") + ", " + ArrivalName(a);
			switch (fn)
			{
			case 0:
				{
					const HeaterMode m = h.GetMode();
					Expect(m == ((online) ? HeaterMode::heating : HeaterMode::offline), name, (online) ? "expected the reported mode" : "expected offline");
				}
				break;
			case 1:
				{
					const float t = h.GetTemperature();
					Expect(t == ((online) ? 205.0f : SensorTemperature), name, (online) ? "expected the reported temperature" : "expected the local sensor reading");
				}
				break;
			default:
				{
					const float p = h.GetAveragePWM();
					Expect(p == ((online) ? 128.0f / 255.0f : 0.0f), name, (online) ? "expected the reported PWM" : "expected 0");
				}
				break;
			}
			writerAfterClockRead = nullptr;
		}
	}
}

static void FanCases() noexcept
{
	for (Arrival a : { Arrival::fresh, Arrival::tooOld, Arrival::newerDuringCheck })
	{
		RemoteFan f;
		millisNow = 2000000;
		f.sensorsMonitored.nonEmpty = true;
		f.lastRpm = 1200;
		f.lastPwm = 0.5;
		f.whenLastReportReceived = (a == Arrival::tooOld) ? millisNow - 3000 : millisNow - 250;
		if (a == Arrival::newerDuringCheck)
		{
			// RemoteFan::UpdateFromRemote (src/Fans/RemoteFan.cpp), in the CanReceiver task
			writerAfterClockRead = [&f]() { f.lastRpm = 1200; f.lastPwm = 0.5; f.whenLastReportReceived = millis(); };
		}
		const bool running = f.Check(true);
		const bool online = (a != Arrival::tooOld);
		Expect(running == online && f.lastRpm == ((online) ? 1200 : -1), std::string("RemoteFan::Check, ") + ArrivalName(a),
				(online) ? "expected the reported RPM and a running thermostatic fan" : "expected RPM -1 and no running fan");
		writerAfterClockRead = nullptr;
	}
}

static void TachoCases() noexcept
{
	for (Arrival a : { Arrival::fresh, Arrival::tooOld, Arrival::newerDuringCheck })
	{
		LocalFan f;
		ticksNow = 3000000000u;
		f.tachoMultiplier = StepClockRate * 30;				// RPM = multiplier/interval, 2 pulses per revolution and 1 pulse per interval here
		f.tachoInterval = StepClockRate / 40;				// 1200 RPM
		f.tachoLastResetTime = (a == Arrival::tooOld) ? ticksNow - 4 * StepClockRate : ticksNow - StepClockRate / 4;
		if (a == Arrival::newerDuringCheck)
		{
			// LocalFan::Interrupt (src/Fans/LocalFan.cpp), the tacho ISR
			writerAfterClockRead = [&f]() { const uint32_t now = StepTimer::GetTimerTicks(); f.tachoInterval = now - f.tachoLastResetTime; f.tachoLastResetTime = now; };
		}
		const int32_t rpm = f.GetRPM();
		const bool online = (a != Arrival::tooOld);
		Expect((online) ? rpm > 0 : rpm == 0, std::string("LocalFan::GetRPM, ") + ArrivalName(a), (online) ? "expected a non-zero RPM" : "expected 0");
		writerAfterClockRead = nullptr;
	}
}

static void FilamentCases() noexcept
{
	for (Arrival a : { Arrival::fresh, Arrival::tooOld, Arrival::newerDuringCheck })
	{
		for (int fn = 0; fn < 3; ++fn)
		{
			FilamentMonitor m;
			millisNow = 4000000;
			m.remotePresenceValid = m.remotePresence = m.remoteMotion = true;
			m.whenRemoteLiveBitsReceived = (a == Arrival::tooOld) ? millisNow - 6000 : millisNow - 1000;
			if (a == Arrival::newerDuringCheck)
			{
				// FilamentMonitor::UpdateRemoteFilamentStatus (src/FilamentMonitors/FilamentMonitor.cpp), in the CanReceiver task
				writerAfterClockRead = [&m]() { m.remotePresenceValid = m.remotePresence = m.remoteMotion = true; m.whenRemoteLiveBitsReceived = millis(); };
			}
			const bool online = (a != Arrival::tooOld);
			const bool result = (fn == 0) ? m.FilamentPresenceKnown() : (fn == 1) ? m.IsFilamentPresent() : m.IsMotionDetected();
			const std::string name = std::string("FilamentMonitor::") + ((fn == 0) ? "FilamentPresenceKnown" : (fn == 1) ? "IsFilamentPresent" : "IsMotionDetected")
										+ ", " + ArrivalName(a);
			Expect(result == online, name, (online) ? "expected the reported state (true)" : "expected false");
			writerAfterClockRead = nullptr;
		}
	}
}

static void PulsedCases() noexcept
{
	for (Arrival a : { Arrival::fresh, Arrival::tooOld, Arrival::newerDuringCheck })
	{
		for (int fn = 0; fn < 2; ++fn)
		{
			PulsedFilamentMonitor m;
			millisNow = 5000000;
			m.samplesReceived = 10;
			// Enough extrusion since the last sync for the "sync overdue" branch of Check, but not for the regular comparison
			m.extrusionCommandedThisSegment = 3.0;
			m.extrusionCommandedSinceLastSync = 8.0;
			// A pulse 100 ms ago is recent for both checks (sync overdue after 220 ms, motion latched for 500 ms)
			m.lastMeasurementTime = (a == Arrival::tooOld) ? millisNow - 600 : millisNow - 100;
			if (a == Arrival::newerDuringCheck)
			{
				// PulsedFilamentMonitor::Interrupt, the real pin interrupt
				writerAfterClockRead = [&m]() { (void)m.Interrupt(); };
			}
			const bool recent = (a != Arrival::tooOld);
			std::string name = std::string("PulsedFilamentMonitor::") + ((fn == 0) ? "Check" : "IsLocalMotionDetected") + ", " + ArrivalName(a);
			if (fn == 0)
			{
				(void)m.Check(true, false, 0, 0.0);
				const bool overdue = m.overdueComparisons != 0;
				Expect(overdue == !recent, name, (recent) ? "expected no \"sync overdue\" comparison" : "expected the \"sync overdue\" comparison");
			}
			else
			{
				const bool moving = m.IsLocalMotionDetected();
				Expect(moving == recent, name, (recent) ? "expected motion detected" : "expected no motion");
			}
			writerAfterClockRead = nullptr;
		}
	}
}

int main()
{
	HeaterCases();
	FanCases();
	TachoCases();
	FilamentCases();
	PulsedCases();
	std::printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
