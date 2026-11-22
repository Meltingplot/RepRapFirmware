// Host test of the step timer of LocalZProbe against the deletion of the probe, compiled from the real text that the Makefile
// extracts: the class declaration of LocalZProbe (src/Endstops/LocalZProbe.h), its destructor, SendProgram, TimerInterrupt and
// Interrupt (src/Endstops/LocalZProbe.cpp), the pending list code of StepTimer (src/Movement/StepTimer.cpp, with the real
// src/Movement/StepTimer.h), EndstopsManager::ProgramZProbe (M672) and SetZProbeDefaults (M502), and DeleteObject.
//
// M672 kicks off sending a program to the probe and returns at once (SendProgram). The step ISR then sends the program bit by
// bit at 1000 bits per second, calling LocalZProbe::Interrupt through the probe's StepTimer, which stays in StepTimer::pendingList
// until the last bit has gone out: 14 bit times per byte, up to 112ms for the 8 bytes that M672 accepts. M558 with P or C and
// M502 delete the probe in the main task. If the probe's timer is still pending then, the step ISR calls back the deleted probe;
// or, when M558 creates a new LocalZProbe and the free list hands it the same memory, the StepTimer constructor resets the pending
// timer, so the step ISR calls a null callback and the timers behind it in the pending list are lost.
//
// The test plays the step ISR between the statements of the main task. Before every interrupt it walks the pending list and
// fails if a timer in it lies in the memory of a deleted probe or has no callback, and then stops the ISR, so the test never calls
// a deleted probe or a null callback. The free list keeps the memory of the deleted probes, as the firmware's does.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <RepRapFirmware.h>

#define private public				// the test walks StepTimer::pendingList
#include <Movement/StepTimer.h>
#undef private

// RRFLibraries src/General/FreelistManager.h: one free list per object size rounded up to 8 bytes, last in first out, with the
// link written over the start of the released object. Plus the bookkeeping of the test: which memory is on a free list.
struct ReleasedBlock
{
	const char *start;
	size_t size;
};

static std::vector<ReleasedBlock> releasedBlocks;

namespace FreelistManager
{
	template<size_t Sz> class Freelist
	{
	public:
		static void *AllocateItem() noexcept
		{
			if (freelist != nullptr)
			{
				void * const p = freelist;
				freelist = *reinterpret_cast<void **>(p);
				for (size_t i = 0; i < releasedBlocks.size(); ++i)
				{
					if (releasedBlocks[i].start == p)
					{
						releasedBlocks.erase(releasedBlocks.begin() + i);
						break;
					}
				}
				return p;
			}
			return ::operator new(Sz);
		}

		static void ReleaseItem(void *p) noexcept
		{
			*reinterpret_cast<void **>(p) = freelist;
			freelist = p;
			releasedBlocks.push_back({ static_cast<const char *>(p), Sz });
		}

	private:
		static void *freelist;
	};

	template<size_t Sz> void *Freelist<Sz>::freelist = nullptr;

	inline constexpr size_t RoundedUpSize(size_t rawSize) noexcept
	{
		constexpr size_t sizeIncrement = 8;
		return ((rawSize + (sizeIncrement - 1u)) & ~(sizeIncrement - 1u));
	}

	template<class T> inline void *Allocate() noexcept
	{
		return Freelist<RoundedUpSize(sizeof(T))>::AllocateItem();
	}

	template<class T> inline void Release(void *p) noexcept
	{
		Freelist<RoundedUpSize(sizeof(T))>::ReleaseItem(p);
	}
}

#define DECLARE_FREELIST_NEW_DELETE(_Type) \
void* operator new(size_t /* sz */) noexcept { return FreelistManager::Allocate<_Type>(); } \
void operator delete(void* p) noexcept { FreelistManager::Release<_Type>(p); }

// CANlib src/GCodeResult.h, the values used here
enum class GCodeResult : uint8_t { notFinished, ok, warning, warningNotSupported, error };

// src/Endstops/EndstopDefs.h:72, the value used here
enum class ZProbeType : uint8_t { none = 0 };

// RRFLibraries StringRef, as far as ProgramZProbe uses it
class StringRef
{
public:
	explicit StringRef(std::string& s) noexcept : str(s) { }
	void copy(const char *s) const noexcept { str = s; }

private:
	std::string& str;
};

// RRFLibraries RTOSIface.h: ReadWriteLock, ReadLocker and WriteLocker, compile-only; the test runs one task
class ReadWriteLock
{
};

class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock&) noexcept { }
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock&) noexcept { }
};

// src/ObjectModel/ObjectModel.h: the lifetime lock that SetZProbeDefaults takes
class ObjectModel
{
public:
	static ReadWriteLock lifetimeLock;
};

ReadWriteLock ObjectModel::lifetimeLock;

// src/GCodes/GCodeBuffer/GCodeBuffer.h, as far as ProgramZProbe uses it: M672 K<k> S<byte>:<byte>...
class GCodeBuffer
{
public:
	bool Seen(char c) noexcept { return (c == 'K') ? hasK : (c == 'S') ? !program.empty() : false; }
	uint32_t GetLimitedUIValue(char, uint32_t) noexcept { return k; }
	void GetUnsignedArray(uint32_t arr[], size_t& length, bool) noexcept
	{
		if (program.size() < length)
		{
			length = program.size();
		}
		for (size_t i = 0; i < length; ++i)
		{
			arr[i] = program[i];
		}
	}

	bool hasK = false;
	uint32_t k = 0;
	std::vector<uint32_t> program;
};

// The step clock and the compare interrupt of the step timer, the hardware part of src/Movement/StepTimer.cpp
static StepTimer::Ticks now;
static StepTimer::Ticks compareTime;
static bool interruptEnabled;
static unsigned int timerChecksInThisInterrupt;

// src/Hardware/IoPorts.h, as far as LocalZProbe uses it. Release() makes the port invalid and WriteDigital() does nothing on an
// invalid port (src/Hardware/IoPorts.cpp:154-167 and 485-491). The test records the writes with the step clock.
class IoPort
{
public:
	void Release() noexcept { valid = false; }
	void WriteDigital(bool high) const noexcept;

private:
	bool valid = true;												// as if M558 C had assigned a pin
};

struct PortWrite
{
	const IoPort *port;
	StepTimer::Ticks when;
	bool high;
};

static std::vector<PortWrite> portWrites;

void IoPort::WriteDigital(bool high) const noexcept
{
	if (valid)
	{
		portWrites.push_back({ this, now, high });
	}
}

// src/Endstops/ZProbe.h, as far as LocalZProbe and the extracted EndstopsManager functions use it
class ZProbe
{
public:
	ZProbe(unsigned int num, ZProbeType p_type) noexcept : type(p_type), number(num) { }
	virtual ~ZProbe() noexcept { }									// as EndstopOrZProbe, src/Endstops/Endstop.h:30

	virtual void SetIREmitter(bool on) const noexcept = 0;
	virtual int32_t GetRawReading() const noexcept = 0;
	virtual bool SetProbing(bool isProbing) noexcept = 0;
	virtual GCodeResult AppendPinNames(const StringRef& str) noexcept = 0;
	virtual GCodeResult Configure(GCodeBuffer&, const StringRef&, bool&) THROWS(GCodeException) { return GCodeResult::ok; }
	virtual GCodeResult SendProgram(const uint32_t[], size_t, const StringRef&) noexcept { return GCodeResult::error; }
	virtual GCodeResult GetCalibratedReading(float&) const noexcept { return GCodeResult::error; }

	void SetDefaults() noexcept { }

protected:
	ZProbeType type;
	unsigned int number;
};

// The real class declaration of LocalZProbe
#include <LocalZProbeClass.inc>

// The other virtual functions of LocalZProbe, which the test does not use; their definitions in LocalZProbe.cpp need the ADC and
// the G-code parser
int32_t LocalZProbe::GetRawReading() const noexcept { return 0; }
bool LocalZProbe::SetProbing(bool) noexcept { return true; }
GCodeResult LocalZProbe::AppendPinNames(const StringRef&) noexcept { return GCodeResult::ok; }
GCodeResult LocalZProbe::Configure(GCodeBuffer&, const StringRef&, bool&) THROWS(GCodeException) { return GCodeResult::ok; }
void LocalZProbe::SetIREmitter(bool) const noexcept { }
GCodeResult LocalZProbe::GetCalibratedReading(float&) const noexcept { return GCodeResult::error; }

// src/Endstops/EndstopsManager.h, the members that ProgramZProbe and SetZProbeDefaults use
class EndstopOrZProbe;															// only the pointer type of activeEndstops is used

class EndstopsManager
{
public:
	void SetZProbeDefaults() noexcept;
	GCodeResult ProgramZProbe(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	static ReadWriteLock zProbesLock;
	ZProbe *_ecv_from _ecv_null zProbes[MaxZProbes] = { };
	EndstopOrZProbe *_ecv_from _ecv_null volatile activeEndstops = nullptr;	// SetZProbeDefaults clears it (#56), EndstopsManager.h:103
};

ReadWriteLock EndstopsManager::zProbesLock;

StepTimer::Ticks StepTimer::GetTimerTicks() noexcept
{
	return now;
}

// As src/Movement/StepTimer.cpp:227-250: return true if the time is less than MinInterruptInterval away or has passed, else set
// the compare register and enable the interrupt
bool StepTimer::ScheduleTimerInterrupt(uint32_t tim) noexcept
{
	constexpr uint32_t MinInterruptInterval = (6 * StepClockRate)/1000000;			// src/Movement/MoveTiming.h:32

	// StepTimer::Interrupt and the callbacks that it runs call this for every timer that they look at. The step clock of the test
	// stands still during an interrupt, so a step ISR that never ends shows up as an endless number of calls.
	if (++timerChecksInThisInterrupt > 1000)
	{
		printf("FAIL the step ISR at %" PRIu32 " does not end\n", now);
		exit(1);
	}

	if ((int32_t)(tim - now) < (int32_t)MinInterruptInterval)
	{
		return true;
	}
	compareTime = tim;
	interruptEnabled = true;
	return false;
}

StepTimer *_ecv_null volatile StepTimer::pendingList = nullptr;							// src/Movement/StepTimer.cpp:30

#include <StepTimer.inc>
#include <LocalZProbe.inc>
#include <EndstopsManager.inc>

constexpr StepTimer::Ticks BitTime = StepClockRate/1000;		// LocalZProbe::bitsPerSecond, src/Endstops/LocalZProbe.h:58

// When the other client of the step timer is due after the start of a case: after the 28 bit times of the 2-byte programs, and off
// the bit times of the probes, so that no two timers are due at the same tick (see README.md)
constexpr StepTimer::Ticks OtherDue = 40 * BitTime + 7;

static bool InMemoryOf(const void *p, const char *start, size_t size) noexcept
{
	const char *const cp = static_cast<const char *>(p);
	return cp >= start && cp < start + size;
}

static bool InReleasedMemory(const void *p) noexcept
{
	for (const ReleasedBlock& b : releasedBlocks)
	{
		if (InMemoryOf(p, b.start, b.size))
		{
			return true;
		}
	}
	return false;
}

static bool InProbe(const void *p, const LocalZProbe *probe) noexcept
{
	return InMemoryOf(p, reinterpret_cast<const char *>(probe), sizeof(LocalZProbe));
}

// Check the timers in the pending list of the step timer. Return why the step ISR must not run them, or an empty string.
static std::string CheckPendingList() noexcept
{
	unsigned int deleted = 0, noCallback = 0, count = 0;
	for (const StepTimer *_ecv_null t = StepTimer::pendingList; t != nullptr; t = t->next)
	{
		if (InReleasedMemory(t))
		{
			++deleted;
		}
		else if (t->callback == nullptr)
		{
			++noCallback;
		}
		if (++count > 100)
		{
			return "the pending list of the step timer has a loop";
		}
	}

	std::string why;
	if (deleted != 0)
	{
		why = std::to_string(deleted) + " step timer(s) of deleted probes still pending, so the step ISR would call back a deleted probe";
	}
	if (noCallback != 0)
	{
		why += std::string((why.empty()) ? "" : "; ") + std::to_string(noCallback)
			+ " pending step timer(s) with a null callback, reset by the constructor of the new probe, so the step ISR would call a null pointer";
	}
	return why;
}

static bool IsPending(const StepTimer& timer) noexcept
{
	for (const StepTimer *_ecv_null t = StepTimer::pendingList; t != nullptr; t = t->next)
	{
		if (t == &timer)
		{
			return true;
		}
	}
	return false;
}

static unsigned int NumPendingTimersOf(const LocalZProbe *probe) noexcept
{
	unsigned int n = 0;
	for (const StepTimer *_ecv_null t = StepTimer::pendingList; t != nullptr; t = t->next)
	{
		if (InProbe(t, probe))
		{
			++n;
		}
	}
	return n;
}

// Run the step ISR as STEP_TC_HANDLER does (src/Movement/StepTimer.cpp:435-459), at every compare match up to the given time.
// Before every interrupt check the pending list; if a timer in it must not be called, stop the ISR and return why.
static std::string RunStepIsrUntil(StepTimer::Ticks until) noexcept
{
	while (interruptEnabled && (int32_t)(compareTime - until) <= 0)
	{
		now = compareTime;
		interruptEnabled = false;								// the handler disables the interrupt, StepTimer.cpp:443 and 451
		const std::string why = CheckPendingList();
		if (!why.empty())
		{
			StepTimer::pendingList = nullptr;					// so that nothing calls a deleted probe or a null callback
			return why;
		}
		timerChecksInThisInterrupt = 0;
		StepTimer::Interrupt();
	}
	now = until;
	return std::string();
}

// Another client of the step timer, e.g. Move::stepsTimer (src/Movement/Move.h:657). Its callback checks whether it is really
// due, as src/Movement/StepTimer.h:16-19 requires of every client.
static StepTimer otherTimer;
static unsigned int otherCalls = 0;

static void OtherCallback(CallbackParameter) noexcept
{
	if (otherTimer.ScheduleCallbackFromIsr())
	{
		++otherCalls;
	}
}

// The levels that M672 sends for these bytes, as the comment in LocalZProbe::Interrupt gives them:
// [0 0 1 0 b7 b6 b5 b4 /b4 b3 b2 b1 b0 /b0] for each byte
static std::string ExpectedBits(const std::vector<uint32_t>& bytes) noexcept
{
	std::string bits;
	for (uint32_t b : bytes)
	{
		bits += "0010";
		for (int i = 7; i >= 4; --i)
		{
			bits += ((b >> i) & 1) ? '1' : '0';
		}
		bits += ((b >> 4) & 1) ? '0' : '1';
		for (int i = 3; i >= 0; --i)
		{
			bits += ((b >> i) & 1) ? '1' : '0';
		}
		bits += (b & 1) ? '0' : '1';
	}
	return bits;
}

// The level of the probe's ports in the middle of each bit time from start, from the recorded writes ('?' if not yet written)
static std::string SentBits(const LocalZProbe *probe, StepTimer::Ticks start, size_t numBits) noexcept
{
	std::string bits;
	for (size_t i = 0; i < numBits; ++i)
	{
		const StepTimer::Ticks sample = start + i * BitTime + BitTime/2;
		char level = '?';
		for (const PortWrite& w : portWrites)
		{
			if (InProbe(w.port, probe) && (int32_t)(w.when - start) >= 0 && (int32_t)(w.when - sample) <= 0)
			{
				level = (w.high) ? '1' : '0';
			}
		}
		bits += level;
	}
	return bits;
}

static size_t NumWritesOf(const LocalZProbe *probe, size_t fromIndex) noexcept
{
	size_t n = 0;
	for (size_t i = fromIndex; i < portWrites.size(); ++i)
	{
		if (InProbe(portWrites[i].port, probe))
		{
			++n;
		}
	}
	return n;
}

static bool LastWriteOf(const LocalZProbe *probe, StepTimer::Ticks& when, bool& high) noexcept
{
	bool found = false;
	for (const PortWrite& w : portWrites)
	{
		if (InProbe(w.port, probe))
		{
			when = w.when;
			high = w.high;
			found = true;
		}
	}
	return found;
}

// One case: a fresh EndstopsManager, an empty pending list and the step clock at an arbitrary time
class Fixture
{
public:
	Fixture() noexcept
	{
		StepTimer::pendingList = nullptr;
		interruptEnabled = false;
		now = 0x12345678;
		portWrites.clear();
		otherTimer.next = nullptr;
		otherTimer.active = false;
		otherTimer.SetCallback(OtherCallback, CallbackParameter());
		otherCalls = 0;
	}

	~Fixture()
	{
		StepTimer::pendingList = nullptr;							// whatever the case left there, the next case starts afresh
		for (ZProbe *_ecv_null& zp : em.zProbes)
		{
			DeleteObject(zp);
		}
		StepTimer::pendingList = nullptr;
	}

	// M558 K<k> P5 C"..." when there is no probe K<k> yet: EndstopsManager::HandleM558 creates a LocalZProbe
	// (src/Endstops/EndstopsManager.cpp:811) and stores it when Configure succeeds (:820)
	LocalZProbe *NewProbe(unsigned int k) noexcept
	{
		LocalZProbe *const newProbe = new LocalZProbe(k);
		em.zProbes[k] = newProbe;
		return newProbe;
	}

	// M558 K<k> P5 C"..." again: HandleM558 deletes the old probe first (:757), then creates and stores the new one as above
	LocalZProbe *ReplaceProbe(unsigned int k) noexcept
	{
		DeleteObject(em.zProbes[k]);
		return NewProbe(k);
	}

	// M672 K<k> S<byte>:<byte>...
	GCodeResult M672(unsigned int k, const std::vector<uint32_t>& bytes) noexcept
	{
		GCodeBuffer gb;
		gb.hasK = true;
		gb.k = k;
		gb.program = bytes;
		std::string replyText;
		const StringRef reply(replyText);
		return em.ProgramZProbe(gb, reply);
	}

	// Check the pending list after the main task has deleted a probe, then run the step ISR until the other client of the step
	// timer has been called back. Return why that failed, or an empty string.
	std::string IsrAfterDelete(StepTimer::Ticks otherDue, const LocalZProbe *newProbe) noexcept
	{
		std::string why = CheckPendingList();
		if (!IsPending(otherTimer))
		{
			why += std::string((why.empty()) ? "" : "; ") + "the step timer of the other client fell out of the pending list";
		}
		if (!why.empty())
		{
			StepTimer::pendingList = nullptr;
			return why;
		}

		const size_t writesBefore = portWrites.size();
		why = RunStepIsrUntil(otherDue + 10 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		if (otherCalls != 1)
		{
			return "the other client of the step timer was called back " + std::to_string(otherCalls) + " times, expected once";
		}
		if (newProbe != nullptr && NumWritesOf(newProbe, writesBefore) != 0)
		{
			return "the step ISR wrote to the ports of the new probe, which has no program to send";
		}
		return std::string();
	}

	EndstopsManager em;
};

static unsigned int passed = 0, failed = 0;

template<class F> static void Case(const char *name, F test) noexcept
{
	std::string why;
	{
		Fixture fx;
		why = test(fx);
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
	const std::vector<uint32_t> program = { 0xA5, 0x3C };

	// The path that sends a program, so that the cases below delete the probe while its timer is really pending
	Case("M672 returns before the program is sent, and the step ISR sends it in 14 bit times per byte", [&](Fixture& fx) {
		LocalZProbe *const probe = fx.NewProbe(0);
		const StepTimer::Ticks start = now;
		if (fx.M672(0, program) != GCodeResult::ok)
		{
			return std::string("M672 failed");
		}
		if (NumPendingTimersOf(probe) != 1)
		{
			return std::string("the step timer of the probe is not pending when M672 returns");
		}
		std::string why = RunStepIsrUntil(start + 50 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		const std::string sent = SentBits(probe, start, 14 * program.size());
		if (sent != ExpectedBits(program))
		{
			return "sent " + sent + ", expected " + ExpectedBits(program);
		}
		StepTimer::Ticks when = 0;
		bool high = true;
		if (!LastWriteOf(probe, when, high) || high || when != start + 14 * program.size() * BitTime)
		{
			return std::string("the line did not return to 0 at the end of the last byte");
		}
		if (NumPendingTimersOf(probe) != 0)
		{
			return std::string("the step timer of the probe is still pending after the program was sent");
		}
		return std::string();
	});

	// SendProgram cancels the timer before it starts again (LocalZProbe.cpp:154), so a second M672 restarts the send
	Case("M672 while a program is being sent starts the new one", [&](Fixture& fx) {
		LocalZProbe *const probe = fx.NewProbe(0);
		const StepTimer::Ticks start = now;
		(void)fx.M672(0, program);
		std::string why = RunStepIsrUntil(start + 5 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		const std::vector<uint32_t> second = { 0x0F };
		const StepTimer::Ticks secondStart = now;
		(void)fx.M672(0, second);
		if (NumPendingTimersOf(probe) != 1)
		{
			return "the step timer of the probe is pending " + std::to_string(NumPendingTimersOf(probe)) + " times, expected once";
		}
		why = RunStepIsrUntil(secondStart + 30 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		const std::string sent = SentBits(probe, secondStart, 14 * second.size());
		return (sent == ExpectedBits(second)) ? std::string() : "sent " + sent + ", expected " + ExpectedBits(second);
	});

	// The probe is replaced by M558 with the same type while M672 is sending. The free list hands the new probe the memory of the
	// old one, so the StepTimer constructor resets the timer that is still in the pending list.
	Case("M558 P5 C replaces the probe while M672 is sending", [&](Fixture& fx) {
		LocalZProbe *const oldProbe = fx.NewProbe(0);
		const StepTimer::Ticks start = now;
		otherTimer.ScheduleCallback(start + OtherDue);
		(void)fx.M672(0, program);
		std::string why = RunStepIsrUntil(start + 5 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		LocalZProbe *const newProbe = fx.ReplaceProbe(0);
		if (newProbe != oldProbe)
		{
			return std::string("test setup: the free list did not hand the new probe the memory of the old one");
		}
		return fx.IsrAfterDelete(start + OtherDue, newProbe);
	});

	// The probe is deleted by M558 while M672 is sending, and its memory stays on the free list (M558 P0 or P10 create a
	// DummyZProbe or MotorStallZProbe instead, src/Endstops/EndstopsManager.cpp:763 and 767)
	Case("M558 P deletes the probe while M672 is sending", [&](Fixture& fx) {
		(void)fx.NewProbe(0);
		const StepTimer::Ticks start = now;
		otherTimer.ScheduleCallback(start + OtherDue);
		(void)fx.M672(0, program);
		std::string why = RunStepIsrUntil(start + 5 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		DeleteObject(fx.em.zProbes[0]);
		return fx.IsrAfterDelete(start + OtherDue, nullptr);
	});

	// M502 deletes all probes (EndstopsManager::SetZProbeDefaults) while M672 is sending to two of them
	Case("M502 deletes the probes while M672 is sending", [&](Fixture& fx) {
		(void)fx.NewProbe(0);
		(void)fx.NewProbe(1);
		const StepTimer::Ticks start = now;
		otherTimer.ScheduleCallback(start + OtherDue);
		(void)fx.M672(0, program);
		now += 100;									// the next command comes later; at the same tick the bit times of the two probes would coincide
		(void)fx.M672(1, program);
		std::string why = RunStepIsrUntil(start + 5 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		fx.em.SetZProbeDefaults();
		return fx.IsrAfterDelete(start + OtherDue, nullptr);
	});

	// The same replacement after the program has been sent, when the timer is no longer pending
	Case("M558 P5 C replaces the probe after M672 has finished sending", [&](Fixture& fx) {
		(void)fx.NewProbe(0);
		const StepTimer::Ticks start = now;
		otherTimer.ScheduleCallback(start + OtherDue);
		(void)fx.M672(0, program);
		std::string why = RunStepIsrUntil(start + 30 * BitTime);
		if (!why.empty())
		{
			return why;
		}
		LocalZProbe *const newProbe = fx.ReplaceProbe(0);
		return fx.IsrAfterDelete(start + OtherDue, newProbe);
	});

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
