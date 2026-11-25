// Host test of how EndstopsManager (src/Endstops/EndstopsManager.cpp) deletes Z probes that may still be in its list of active
// endstops, compiled from the real text of AddToActive, EnableZProbe, CheckEndstops, SetZProbeDefaults and HandleM558, and of
// DeleteObject (src/RepRapFirmware.h), which the Makefile extracts.
//
// EndstopsManager::activeEndstops lists the endstops and Z probes of the current move. EnableZProbe puts a probe in it before a
// probing move (G30, G38.x, ...). Only CheckEndstops takes the probe out again, when it triggers; a probing move that ends without
// a trigger leaves it in the list until the next move that enables endstops rebuilds the list. Two contexts with a higher
// priority than the main task, which runs M558 and M502, walk the list and call CheckTriggered on every entry:
// - the step ISR, while a move that checks endstops executes (Move::CheckEndstops in src/Movement/Move.cpp);
// - the CAN receiver task, after every endstop, Z probe or GpIn state change that an expansion board reports
//   (src/CAN/CommandProcessor.cpp -> Move::OnEndstopOrZProbeStatesChanged), but only while a segment that checks endstops
//   executes (upstream 190e3f174).
// So a probe may stay in the list after its deletion, as long as no reader walks the list before the next move that checks
// endstops has rebuilt it: M558 with P and M502 lock movement and wait for standstill before they delete probes. The test plays
// these readers while a probe is deleted (in its destructor, before and after its memory is freed) and at the next remote input
// change. It never calls a deleted object itself: the reader first compares each list entry with the probes deleted so far, and
// their memory is kept until the case ends.

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from
#define THROWS(...)		noexcept(false)

// As on the Duet 3 MB6HC of the lab printer
#define SUPPORT_CAN_EXPANSION		1
#define SUPPORT_SCANNING_PROBES		1						// SUPPORT_CAN_EXPANSION, src/Config/Pins.h
constexpr size_t MaxZProbes = 8;							// CANlib src/Duet3Common.h
constexpr size_t StringLength20 = 20;

#include "DeleteObject.inc"

// Just what the functions under test use
enum class GCodeResult : uint8_t { notFinished, ok, warning, error };			// the values used here, from CANlib src/GCodeResult.h

inline constexpr bool Succeeded(GCodeResult rslt) noexcept						// as in CANlib src/GCodeResult.h
{
	return rslt == GCodeResult::ok || rslt == GCodeResult::warning;
}

class GCodeException
{
public:
	explicit GCodeException(const char *_ecv_array msg) noexcept : message(msg) { }
	const char *_ecv_array message;
};

class StringRef
{
public:
	StringRef(char *_ecv_array p, size_t l) noexcept : buf(p), len(l) { }
	void copy(const char *_ecv_array s) const noexcept { std::snprintf(buf, len, "%s", s); }
	int printf(const char *_ecv_array fmt, ...) const noexcept __attribute__ ((format (printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = std::vsnprintf(buf, len, fmt, vargs);
		va_end(vargs);
		return ret;
	}
	const char *_ecv_array c_str() const noexcept { return buf; }

private:
	char *_ecv_array buf;
	size_t len;
};

template<size_t Len> class String
{
public:
	String() noexcept { data[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(data, sizeof(data)); }

private:
	char data[Len + 1];
};

// A parsed command, e.g. GCodeBuffer("M558 K0 P8 C\"21.io0.in\" H5")
class GCodeBuffer
{
public:
	explicit GCodeBuffer(const char *_ecv_array cmd) noexcept
	{
		const std::string s(cmd);
		size_t i = s.find(' ');
		while (i != std::string::npos && i < s.size())
		{
			while (i < s.size() && s[i] == ' ') { ++i; }
			if (i == s.size()) { break; }
			const char letter = s[i++];
			std::string value;
			if (i < s.size() && s[i] == '"')
			{
				const size_t end = s.find('"', i + 1);
				value = s.substr(i + 1, end - i - 1);
				i = end + 1;
			}
			else
			{
				const size_t end = s.find(' ', i);
				value = s.substr(i, (end == std::string::npos) ? std::string::npos : end - i);
				i = end;
			}
			params[letter] = value;
		}
	}

	bool Seen(char c) noexcept
	{
		if (params.count(c) == 0) { return false; }
		lastSeen = c;
		return true;
	}

	int8_t GetCommandFraction() const noexcept { return -1; }

	uint32_t GetLimitedUIValue(char c, uint32_t maxValuePlusOne) THROWS(GCodeException)
	{
		const uint32_t val = (uint32_t)std::strtoul(params.at(c).c_str(), nullptr, 10);
		if (val >= maxValuePlusOne) { throw GCodeException("value out of range"); }
		return val;
	}

	bool TryGetLimitedUIValue(char c, uint32_t& val, bool& seen, uint32_t maxValuePlusOne) THROWS(GCodeException)	// as in GCodeBuffer.cpp
	{
		if (Seen(c))
		{
			val = GetLimitedUIValue(c, maxValuePlusOne);
			seen = true;
			return true;
		}
		return false;
	}

	void GetReducedString(const StringRef& str) THROWS(GCodeException) { str.copy(params.at(lastSeen).c_str()); }	// the pin names used here have nothing to remove

private:
	std::map<char, std::string> params;
	char lastSeen = 0;
};

class ReadWriteLock { };

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock&) noexcept { }
};

// src/ObjectModel/ObjectModel.h: the lifetime lock that SetZProbeDefaults and HandleM558 take, compile-only like the other locks
class ObjectModel
{
public:
	static ReadWriteLock lifetimeLock;
};

ReadWriteLock ObjectModel::lifetimeLock;

typedef uint8_t CanAddress;

namespace CanInterface
{
	CanAddress GetCanAddress() noexcept { return 0; }							// the main board
}

namespace IoPort
{
	// As IoPort::RemoveBoardAddress in src/Hardware/IoPorts.cpp for the pin names used here: "21.io0.in" is on board 21
	CanAddress RemoveBoardAddress(const StringRef& portName) noexcept
	{
		const std::string s(portName.c_str());
		const size_t dot = s.find('.');
		if (dot != std::string::npos && dot != 0 && s.find_first_not_of("0123456789") == dot)
		{
			portName.copy(s.substr(dot + 1).c_str());
			return (CanAddress)std::strtoul(s.c_str(), nullptr, 10);
		}
		return CanInterface::GetCanAddress();
	}
}

class GCodes
{
public:
	// The G30 of another channel holds the movement lock while its move executes, so this fails until that move has finished
	bool LockAllMovementSystemsAndWaitForStandstill(GCodeBuffer&) noexcept { ++lockCalls; return atStandstill; }
	GCodeResult HandleM558Subcommand(GCodeBuffer&, const StringRef&, unsigned int) THROWS(GCodeException) { return GCodeResult::ok; }

	bool atStandstill = true;
	unsigned int lockCalls = 0;
};

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return gcodes; }

	GCodes gcodes;
};

static RepRap reprap;

class Move
{
public:
	static void CreateLaserTask() noexcept { }
};

// As in src/Endstops/EndstopDefs.h, with the fields that CheckEndstops uses
enum class EndstopHitAction : uint8_t
{
	none = 0,
	stopDriver = 1,
	stopAxis = 2,
	stopAll = 3
};

struct EndstopHitDetails
{
	EndstopHitAction GetAction() const noexcept { return action; }

	EndstopHitAction action = EndstopHitAction::none;
	bool setAxisLow = false;
	bool setAxisHigh = false;
	bool isZProbe = false;
};

enum class ZProbeType : uint8_t												// as in src/Endstops/EndstopDefs.h
{
	none = 0,
	analog = 1,
	dumbModulated = 2,
	alternateAnalog_obsolete = 3,
	endstopSwitch_obsolete = 4,
	digital = 5,
	e1Switch_obsolete = 6,
	zSwitch_obsolete = 7,
	unfilteredDigital = 8,
	blTouch = 9,
	zMotorStall = 10,
	scanningAnalog = 11,
	loadCell = 12,
	numTypes = 13
};

// As in src/Endstops/Endstop.h: the base class of the entries of the active list
class EndstopOrZProbe
{
public:
	EndstopOrZProbe() noexcept : next(nullptr) { }
	virtual ~EndstopOrZProbe() noexcept { }

	virtual EndstopHitDetails CheckTriggered() noexcept = 0;
	virtual bool Acknowledge(EndstopHitDetails what) noexcept = 0;
	EndstopOrZProbe *_ecv_from _ecv_null GetNext() const noexcept { return next; }
	void SetNext(EndstopOrZProbe *_ecv_from _ecv_null e) noexcept { next = e; }

private:
	EndstopOrZProbe *_ecv_from _ecv_null next;
};

// The parts of ZProbe (src/Endstops/ZProbe.h) that the functions under test use. CheckTriggered and Acknowledge do what
// ZProbe.cpp does: stop all motion when the probe reads triggered (or no longer triggered when probing away), and leave the list
// once that has been acted on.
class ZProbe : public EndstopOrZProbe
{
public:
	ZProbe(unsigned int num, ZProbeType p_type, const char *_ecv_array p_kind) noexcept : number(num), type(p_type), kind(p_kind) { }
	~ZProbe() noexcept override;
	static void operator delete(void *p) noexcept;

	void PrepareForUse(bool p_probingAway) noexcept { probingAway = p_probingAway; }
	void SetDefaults() noexcept { }
	virtual GCodeResult Configure(GCodeBuffer& gb, const StringRef&, bool& seen) THROWS(GCodeException)
	{
		++configureCalls;
		if (gb.Seen('H')) { seen = true; }
		return GCodeResult::ok;
	}

	EndstopHitDetails CheckTriggered() noexcept override
	{
		EndstopHitDetails rslt;
		if (triggered != probingAway)
		{
			rslt.action = EndstopHitAction::stopAll;
			rslt.isZProbe = true;
		}
		return rslt;
	}

	bool Acknowledge(EndstopHitDetails what) noexcept override { return what.GetAction() == EndstopHitAction::stopAll; }

	unsigned int number;
	ZProbeType type;
	const char *_ecv_array kind;
	bool triggered = false;													// what Stopped() returns
	bool probingAway = false;
	unsigned int configureCalls = 0;
};

class LocalZProbe : public ZProbe
{
public:
	explicit LocalZProbe(unsigned int num) noexcept : ZProbe(num, ZProbeType::none, "LocalZProbe") { }
};

class RemoteZProbe : public ZProbe
{
public:
	RemoteZProbe(unsigned int num, CanAddress bn, ZProbeType p_type) noexcept : ZProbe(num, p_type, "RemoteZProbe"), boardAddress(bn) { }
	GCodeResult Create(const StringRef&, const StringRef&) noexcept { return GCodeResult::ok; }

	CanAddress boardAddress;
};

class DummyZProbe : public ZProbe
{
public:
	explicit DummyZProbe(unsigned int num) noexcept : ZProbe(num, ZProbeType::none, "DummyZProbe") { }
};

class MotorStallZProbe : public ZProbe
{
public:
	explicit MotorStallZProbe(unsigned int num) noexcept : ZProbe(num, ZProbeType::zMotorStall, "MotorStallZProbe") { }
};

// The members of EndstopsManager (src/Endstops/EndstopsManager.h) that the functions under test use
class EndstopsManager
{
public:
	bool EnableZProbe(size_t probeNumber, bool probingAway = false) noexcept;
	EndstopHitDetails CheckEndstops() noexcept;
	void SetZProbeDefaults() noexcept;
	GCodeResult HandleM558(GCodeBuffer& gb, const StringRef &reply) THROWS(GCodeException);
	void AddToActive(EndstopOrZProbe &_ecv_from e) noexcept;

	static ReadWriteLock zProbesLock;

	EndstopOrZProbe *_ecv_from _ecv_null volatile activeEndstops = nullptr;
	ZProbe *_ecv_from _ecv_null zProbes[MaxZProbes] = { };
	bool isHomingMove = false;
};

ReadWriteLock EndstopsManager::zProbesLock;

#include "EndstopsManager.inc"

// The test

static EndstopsManager *emgr = nullptr;
static std::vector<std::string> failures;
static std::vector<void *> freed;											// the deleted probes, whose memory is kept until the case ends
static const void *beingDeleted = nullptr;									// the probe whose destructor is running
static const char *_ecv_array deletionReader = "";							// who can walk the list while a probe is deleted
static unsigned int zProbeStops = 0;										// how often the reader stopped all motion for a Z probe
static bool checkingSegmentExecutes = false;								// a segment that checks endstops is executing

static bool IsDeleted(const void *p) noexcept
{
	return p == beingDeleted || std::find(freed.begin(), freed.end(), p) != freed.end();
}

static const char *_ecv_array const CanReceiver = "the CAN receiver task (remote input change)";
static const char *_ecv_array const StepIsr = "the step ISR";

// One call of Move::CheckEndstops (src/Movement/Move.cpp), from the step ISR or from the CAN receiver task: it calls
// EndstopsManager::CheckEndstops until that returns no action or stopAll. Before that the list is walked here and each entry
// compared with the deleted probes, so that the test never calls a deleted object. The CAN receiver task calls it only while a
// segment that checks endstops executes (Move::OnEndstopOrZProbeStatesChanged), the step ISR only during such a segment anyway.
static void ReaderRuns(const char *_ecv_array who, const char *_ecv_array moment) noexcept
{
	if (who == CanReceiver && !checkingSegmentExecutes)
	{
		return;
	}

	for (const EndstopOrZProbe *e = emgr->activeEndstops; e != nullptr; e = e->GetNext())
	{
		if (IsDeleted(e))
		{
			failures.push_back(std::string(who) + " reaches a deleted Z probe through the active list " + moment);
			return;
		}
	}

	for (;;)
	{
		const EndstopHitDetails hd = emgr->CheckEndstops();
		if (hd.GetAction() == EndstopHitAction::none)
		{
			return;
		}
		if (hd.GetAction() == EndstopHitAction::stopAll)
		{
			if (hd.isZProbe) { ++zProbeStops; }
			return;
		}
	}
}

ZProbe::~ZProbe() noexcept
{
	beingDeleted = this;
	ReaderRuns(deletionReader, "while the probe's destructor runs");
}

void ZProbe::operator delete(void *p) noexcept
{
	ReaderRuns(deletionReader, "before the probe's memory is freed");
	freed.push_back(p);
	beingDeleted = nullptr;
	ReaderRuns(deletionReader, "after the probe's memory has been freed");
}

// The steps of a probing move that touch the active list: EnableZProbe before the move (GCodes4.cpp, e.g. state gridProbing3 of
// G30), then the step ISR checks the endstops while the move executes. Without a trigger the move ends at its end point, and
// WaitForEndstopOrProbingMoveToFinish (GCodes4.cpp) and the states after it leave the list alone, so the probe stays in it.
static void ProbingMoveWithoutTrigger(size_t probeNumber, bool probingAway) noexcept
{
	if (!emgr->EnableZProbe(probeNumber, probingAway))
	{
		failures.push_back("EnableZProbe failed");
	}
	checkingSegmentExecutes = true;
	ReaderRuns(StepIsr, "during the probing move");
	ReaderRuns(CanReceiver, "during the probing move");
	checkingSegmentExecutes = false;
}

static unsigned int passed = 0, failed = 0;

template<class F> static void Case(const char *_ecv_array name, F body) noexcept
{
	EndstopsManager em;
	emgr = &em;
	reprap.gcodes = GCodes();
	failures.clear();
	zProbeStops = 0;
	checkingSegmentExecutes = false;
	deletionReader = CanReceiver;

	std::string why = body(em);
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}

	// Clean up without readers, then free the memory of the deleted probes
	em.activeEndstops = nullptr;
	for (ZProbe *_ecv_null& zp : em.zProbes)
	{
		delete zp;
		zp = nullptr;
	}
	for (void *p : freed)
	{
		::operator delete(p);
	}
	freed.clear();
	emgr = nullptr;

	if (why.empty())
	{
		++passed;
		std::printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		std::printf("FAIL %s: %s\n", name, why.c_str());
	}
}

static std::string Check(bool ok, const char *_ecv_array what) noexcept
{
	return (ok) ? std::string() : std::string(what);
}

static std::string Join(std::initializer_list<std::string> parts) noexcept
{
	std::string ret;
	for (const std::string& s : parts)
	{
		if (!s.empty()) { ret += (ret.empty() ? "" : "; ") + s; }
	}
	return ret;
}

int main()
{
	char replyBuffer[200] = "";
	const StringRef reply(replyBuffer, sizeof(replyBuffer));

	Case("M558 replaces the probe after a G30 that ended without a trigger, and no remote input change reaches the old probe",
		[&](EndstopsManager& em) {
			ZProbe *const old = em.zProbes[0] = new LocalZProbe(0);
			ProbingMoveWithoutTrigger(0, false);
			GCodeBuffer gb("M558 K0 P8 C\"io3.in\" H5 F120 T6000");
			const GCodeResult rslt = em.HandleM558(gb, reply);
			ReaderRuns(CanReceiver, "at the next remote input change after M558");
			const bool haveNewProbe = em.zProbes[0] != nullptr && !IsDeleted(em.zProbes[0]);
			if (haveNewProbe)
			{
				ProbingMoveWithoutTrigger(0, false);									// the next G30 rebuilds the list before the readers walk it
			}
			return Join({ Check(rslt == GCodeResult::ok, "M558 did not succeed"),
						  Check(IsDeleted(old), "the old probe was not deleted"),
						  Check(haveNewProbe, "there is no new probe") });
		});

	Case("M558 replaces the probe by one on an expansion board after a G38.4 that never lost contact, and no remote input change reaches the old probe",
		[&](EndstopsManager& em) {
			ZProbe *const old = em.zProbes[0] = new LocalZProbe(0);
			old->triggered = true;													// in contact throughout the move away
			ProbingMoveWithoutTrigger(0, true);
			GCodeBuffer gb("M558 K0 P8 C\"21.io0.in\"");
			const GCodeResult rslt = em.HandleM558(gb, reply);
			ReaderRuns(CanReceiver, "at the next remote input change after M558");
			const RemoteZProbe *const rzp = static_cast<const RemoteZProbe *>(em.zProbes[0]);
			return Join({ Check(rslt == GCodeResult::ok, "M558 did not succeed"),
						  Check(IsDeleted(old), "the old probe was not deleted"),
						  Check(rzp != nullptr && std::string(rzp->kind) == "RemoteZProbe" && rzp->boardAddress == 21, "the new probe is not on board 21") });
		});

	Case("M502 deletes the probes after a G30 K1 that ended without a trigger, and no remote input change reaches them",
		[&](EndstopsManager& em) {
			ZProbe *const old0 = em.zProbes[0] = new LocalZProbe(0);
			ZProbe *const old1 = em.zProbes[1] = new RemoteZProbe(1, 21, ZProbeType::unfilteredDigital);
			ProbingMoveWithoutTrigger(1, false);
			em.SetZProbeDefaults();													// M502 in GCodes2.cpp locks all movement and waits for standstill first
			ReaderRuns(CanReceiver, "at the next remote input change after M502");
			bool allNull = true;
			for (const ZProbe *zp : em.zProbes) { if (zp != nullptr) { allNull = false; } }
			return Join({ Check(IsDeleted(old0) && IsDeleted(old1), "a probe was not deleted"),
						  Check(allNull, "a probe is left") });
		});

	Case("M558 from another channel while a G30 probing move executes waits for standstill, and the probe still stops the move",
		[&](EndstopsManager& em) {
			deletionReader = StepIsr;
			ZProbe *const old = em.zProbes[0] = new LocalZProbe(0);
			if (!em.EnableZProbe(0)) { return std::string("EnableZProbe failed"); }
			checkingSegmentExecutes = true;
			ReaderRuns(StepIsr, "during the probing move");
			reprap.gcodes.atStandstill = false;										// the G30 holds the movement lock until it has finished
			GCodeBuffer gb("M558 K0 P8 C\"io3.in\"");
			const GCodeResult during = em.HandleM558(gb, reply);
			const bool keptDuringMove = (em.zProbes[0] == old && !IsDeleted(old));
			if (keptDuringMove)
			{
				old->triggered = true;
			}
			ReaderRuns(StepIsr, "when the probe triggers after M558");
			const unsigned int stopsWhenTriggered = zProbeStops;
			checkingSegmentExecutes = false;
			reprap.gcodes.atStandstill = true;										// the G30 has finished
			deletionReader = CanReceiver;
			const GCodeResult after = em.HandleM558(gb, reply);
			ReaderRuns(CanReceiver, "at the next remote input change after M558");
			return Join({ Check(during == GCodeResult::notFinished, "M558 did not wait while the probing move executes"),
						  Check(keptDuringMove, "M558 replaced the probe of the executing probing move"),
						  Check(stopsWhenTriggered == 1, "the probe did not stop the probing move"),
						  Check(after == GCodeResult::ok && em.zProbes[0] != old && em.zProbes[0] != nullptr, "M558 did not replace the probe after standstill") });
		});

	Case("M558 without P or C changes the existing probe without waiting for standstill",
		[&](EndstopsManager& em) {
			deletionReader = StepIsr;
			ZProbe *const zp = em.zProbes[0] = new LocalZProbe(0);
			if (!em.EnableZProbe(0)) { return std::string("EnableZProbe failed"); }
			checkingSegmentExecutes = true;
			reprap.gcodes.atStandstill = false;										// a probing move executes
			GCodeBuffer gb("M558 K0 H5 F120");
			const GCodeResult rslt = em.HandleM558(gb, reply);
			return Join({ Check(rslt == GCodeResult::ok, "M558 did not succeed"),
						  Check(em.zProbes[0] == zp && zp->configureCalls == 1, "the existing probe was not configured"),
						  Check(reprap.gcodes.lockCalls == 0, "M558 locked movement"),
						  Check(em.activeEndstops == zp, "the executing probing move no longer checks the probe") });
		});

	Case("a probing move with the probe that M558 created stops when that probe triggers",
		[&](EndstopsManager& em) {
			em.zProbes[0] = new LocalZProbe(0);
			GCodeBuffer gb("M558 K0 P8 C\"io3.in\"");
			const GCodeResult rslt = em.HandleM558(gb, reply);
			ZProbe *const zp = em.zProbes[0];
			if (rslt != GCodeResult::ok || zp == nullptr) { return std::string("M558 did not succeed"); }
			if (!em.EnableZProbe(0)) { return std::string("EnableZProbe failed"); }
			checkingSegmentExecutes = true;
			ReaderRuns(StepIsr, "during the probing move");
			zp->triggered = true;
			ReaderRuns(StepIsr, "when the probe triggers");
			checkingSegmentExecutes = false;
			return Join({ Check(zProbeStops == 1, "the probe did not stop the probing move"),
						  Check(em.activeEndstops == nullptr, "the triggered probe is still in the active list") });
		});

	std::printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
