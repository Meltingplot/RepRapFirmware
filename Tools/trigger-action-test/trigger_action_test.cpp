// Host test of what M581 and M581.1 triggers do when they fire.
//
// Upstream added M581 K<n> (TriggerAction in src/GCodes/TriggerItem.h): K0 emergency stop, K1 pause, K2 run trigger<n>.g, K3 feed hold,
// K4 feed hold and abort. Without K trigger 0 is K0, trigger 1 is K1 and all others are K2. GCodes::CheckTriggers executes K0, K4 and K3
// at once and in that order, before it looks at the trigger channel, and K1 and K2 through the trigger channel.
// The fork adds to M581.1 an action line U"<line>" that runs instead of trigger<n>.g, a lock (a configured emergency stop trigger or trigger
// with an action line refuses changes until the next restart), a fail-safe (an emergency stop trigger whose expression cannot be evaluated fires
// instead of being disabled), and makes a trigger whose action line is just M112 an emergency stop.
//
// Scenarios:
//  1. a trigger whose action line is M112 makes an emergency stop at once, whatever trigger 0 does, also while the trigger channel is busy
//     and through M582
//  2. the lock refuses K
//  3. the lock follows the state: a K-only command that makes a trigger an emergency stop locks it, and so does one that fails after K
//  4. an action line only on a trigger that runs a macro file; trigger 0 with K2 may have one, and it runs through the trigger channel;
//     a hard-wired K deletes an action line given before it
//  5. the fail-safe follows the action, not the trigger number, when the trigger is checked and when it is configured; a trigger that
//     is kept after an error at M581.1 is locked
//  6. what upstream does: K3, K4, K1, K2, the order of the immediate actions and the report
//
// The test compiles the real header src/GCodes/TriggerItem.h and the real text of the functions, which the Makefile extracts at build time.
// TriggerItem::EvaluateExpression is replaced by a script of values and errors per expression. The stand-ins record what the extracted
// functions call.

#include <RepRapFirmware.h>							// stubs/RepRapFirmware.h
#include "TriggerItem.h"							// gen/TriggerItem.h: the header under test

static std::vector<std::string> calls;				// what the extracted functions did, in order
static std::vector<std::string> messages;			// what they reported with Platform::Message

static uint32_t now = 1000;
uint32_t millis() noexcept { return now; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// GCodeBuffer: the parameters of one command line, and what the trigger channel does with an action line

enum class GCodeState : uint8_t { normal, pausing1 };	// two of the states in src/GCodes/GCodeMachineState.h:18

class Mutex { public: unsigned int lockCount = 0; };

class MutexLocker
{
public:
	explicit MutexLocker(Mutex& pm) noexcept : m(pm) { ++m.lockCount; }
	~MutexLocker() { --m.lockCount; }

private:
	Mutex& m;
};

class GCodeBuffer
{
public:
	// Parse a line such as M581.1 T5 P"door" U"M112". A quoted string may contain doubled quotes and spaces; other values end at a space.
	explicit GCodeBuffer(const char *_ecv_array line = "") noexcept
	{
		size_t i = 0;
		while (line[i] == ' ') { ++i; }
		if (line[i] != 0)
		{
			++i;															// the command letter
			while (isDigit(line[i])) { ++i; }
			if (line[i] == '.')
			{
				fraction = (int8_t)std::atoi(line + i + 1);
				++i;
				while (isDigit(line[i])) { ++i; }
			}
		}
		for (;;)
		{
			while (line[i] == ' ') { ++i; }
			if (line[i] == 0) { break; }
			Param param;
			param.letter = (char)toupper(line[i++]);
			if (line[i] == '"')
			{
				param.quoted = true;
				++i;
				while (line[i] != 0 && !(line[i] == '"' && line[i + 1] != '"'))
				{
					if (line[i] == '"') { ++i; }							// a doubled quote
					param.text += line[i++];
				}
				if (line[i] == '"') { ++i; }
			}
			else
			{
				while (line[i] != 0 && line[i] != ' ') { param.text += line[i++]; }
			}
			params.push_back(param);
		}
	}

	// The parameter functions of src/GCodes/GCodeBuffer/GCodeBuffer.h that the extracted functions call
	int8_t GetCommandFraction() const noexcept { return fraction; }

	bool Seen(char c) noexcept
	{
		for (size_t i = 0; i < params.size(); ++i)
		{
			if (params[i].letter == c)
			{
				current = (int)i;
				return true;
			}
		}
		current = -1;
		return false;
	}

	int32_t GetIValue() THROWS(GCodeException)
	{
		const Param& p = Current();
		char *end;
		const long ret = std::strtol(p.text.c_str(), &end, 10);
		if (p.quoted || end == p.text.c_str())
		{
			throw GCodeException(this, -1, "expected integer value");
		}
		return (int32_t)ret;
	}

	uint32_t GetUIValue() THROWS(GCodeException)
	{
		const int32_t ret = GetIValue();
		if (ret < 0)
		{
			throw GCodeException(this, -1, "expected non-negative integer value");
		}
		return (uint32_t)ret;
	}

	// As GCodeBuffer::GetLimitedUIValue (src/GCodes/GCodeBuffer/GCodeBuffer.cpp:687-700) with minValue 0
	uint32_t GetLimitedUIValue(char c, uint32_t maxValuePlusOne) THROWS(GCodeException)
	{
		if (!Seen(c))
		{
			throw GCodeException(this, -1, "missing parameter");
		}
		const uint32_t ret = GetUIValue();
		if (ret >= maxValuePlusOne)
		{
			throw GCodeException(this, -1, "parameter too high");
		}
		return ret;
	}

	// As GCodeBuffer::TryGetLimitedUIValue (src/GCodes/GCodeBuffer/GCodeBuffer.cpp:910-919)
	bool TryGetLimitedUIValue(char c, uint32_t& val, bool& seen, uint32_t maxValuePlusOne) THROWS(GCodeException)
	{
		if (Seen(c))
		{
			val = GetLimitedUIValue(c, maxValuePlusOne);
			seen = true;
			return true;
		}
		return false;
	}

	void GetQuotedString(const StringRef& str, bool allowEmpty = false) THROWS(GCodeException)
	{
		const Param& p = Current();
		if (!p.quoted)
		{
			throw GCodeException(this, -1, "expected a string expression");
		}
		if (p.text.empty() && !allowEmpty)
		{
			throw GCodeException(this, -1, "non-empty string expected");
		}
		str.copy(p.text.c_str());
	}

	void GetUnsignedArray(uint32_t arr[], size_t& length, bool doPad) THROWS(GCodeException)
	{
		const Param& p = Current();
		size_t count = 0;
		const char *s = p.text.c_str();
		for (;;)
		{
			char *end;
			const unsigned long v = std::strtoul(s, &end, 10);
			if (p.quoted || end == s || *s == '-')
			{
				throw GCodeException(this, -1, "expected non-negative integer value");
			}
			if (count == length)
			{
				throw GCodeException(this, -1, "array too long");
			}
			arr[count++] = (uint32_t)v;
			if (*end != ':') { break; }
			s = end + 1;
		}
		length = count;
	}

	// What CheckTriggers does with the trigger channel (src/GCodes/GCodes.cpp, running an action line)
	GCodeState GetState() const noexcept { return state; }
	void PutAndDecode(const char *_ecv_array str) noexcept
	{
		calls.push_back(std::string("PutAndDecode ") + str + ((mutex.lockCount == 0) ? " (without the buffer mutex)" : ""));
	}
	bool CheckMetaCommand(const StringRef& reply) THROWS(GCodeException) { return false; }		// the action lines of this test are M-codes
	void Init() noexcept { calls.push_back("GCodeBuffer::Init"); }

	Mutex mutex;
	GCodeState state = GCodeState::normal;

private:
	struct Param
	{
		char letter = 0;
		bool quoted = false;
		std::string text;
	};

	const Param& Current() const THROWS(GCodeException)
	{
		if (current < 0)
		{
			throw GCodeException(this, -1, "no parameter");
		}
		return params[(size_t)current];
	}

	std::vector<Param> params;
	int current = -1;
	int8_t fraction = -1;							// as GetCommandFraction: -1 if there is none
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The other classes, as far as the extracted functions use them

enum class PrintPausedReason : uint8_t { user = 1, trigger = 4 };	// two of src/Platform/PrintPausedReason.h

class EndstopsManager
{
public:
	bool Stopped(size_t axis) const noexcept { return stopped[axis]; }
	bool stopped[3] = { false, false, false };
};

class GpInputPort
{
public:
	bool GetState() const noexcept { return active; }
	bool active = false;
};

class Platform
{
public:
	EndstopsManager& GetEndstops() noexcept { return endstops; }
	const GpInputPort& GetGpInPort(size_t port) const noexcept { return ports[port]; }
	void Message(MessageType type, const char *_ecv_array msg) noexcept { messages.push_back(msg); }

	EndstopsManager endstops;
	GpInputPort ports[MaxGpInPorts];
};

class PrintMonitor
{
public:
	bool IsPrinting() const noexcept { return printing; }
	bool printing = false;
};

class Move
{
public:
	void FeedHold() noexcept { calls.push_back("FeedHold"); }
	bool IsFeedHoldActive() const noexcept { return held; }
	bool held = false;
};

class StringParser
{
public:
	static bool CallsFileFunction(const char *_ecv_array text, size_t length) noexcept;
	bool CallsFileFunction() const noexcept;

private:
	struct { char buffer[1] = { 0 }; } gb;
	size_t commandStart = 0, commandEnd = 0;
};

class GCodes
{
public:
	GCodes() noexcept
	{
		// As GCodes::Reset (src/GCodes/GCodes.cpp:251, :287-291)
		triggerActionRunning = false;
		for (TriggerItem& tr : triggers)
		{
			tr.Init();
		}
		triggersPending.Clear();
	}

	// The extracted functions
	void CheckTriggers() noexcept;
	GCodeResult ConfigureTrigger(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);
	GCodeResult CheckTrigger(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	size_t GetTotalAxes() const noexcept { return 3; }
	const char *_ecv_array GetAxisLetters() const noexcept { return "XYZ"; }

	// Stand-ins that record their calls
	void DoEmergencyStop() noexcept
	{
		calls.push_back("DoEmergencyStop");
		// The real one calls Reset (src/GCodes/GCodes.cpp:1137), which initialises the triggers and clears the pending ones (:251, :287-291)
		triggerActionRunning = false;
		for (TriggerItem& tr : triggers)
		{
			tr.Init();
		}
		triggersPending.Clear();
	}

	void StopAndAbortMotion(const GCodeBuffer& requester) noexcept
	{
		calls.push_back((&requester == &autoPauseGb) ? "StopAndAbortMotion" : "StopAndAbortMotion (not for the autopause channel)");
	}

	bool DoAsynchronousPause(GCodeBuffer& gb, PrintPausedReason reason, GCodeState newState) noexcept
	{
		calls.push_back((&gb == &triggerGb && reason == PrintPausedReason::trigger && newState == GCodeState::pausing1)
							? "DoAsynchronousPause" : "DoAsynchronousPause (other parameters)");
		return true;
	}

	static constexpr int AsyncSystemMacroCode = -3;		// src/GCodes/GCodes.h:365

	bool DoFileMacro(GCodeBuffer& gb, const char *_ecv_array fileName, bool reportMissing, int codeRunning) noexcept
	{
		calls.push_back(std::string("DoFileMacro ") + fileName
						+ ((&gb == &triggerGb && reportMissing && codeRunning == AsyncSystemMacroCode) ? "" : " (other parameters)"));
		return true;
	}

	void HandleReplyPreserveResult(GCodeBuffer& gb, GCodeResult rslt, const char *_ecv_array reply) noexcept
	{
		calls.push_back(std::string("HandleReplyPreserveResult ") + reply);
	}

	bool IsTriggerBusy() const noexcept { return triggerBusy; }
	bool IsReallyPrinting() const noexcept { return reallyPrinting; }
	bool IsSimulating() const noexcept { return simulating; }
	GCodeBuffer *_ecv_null TriggerGCode() const noexcept { return const_cast<GCodeBuffer *>(&triggerGb); }
	GCodeBuffer *_ecv_null AutoPauseGCode() const noexcept { return const_cast<GCodeBuffer *>(&autoPauseGb); }

	// The members of src/GCodes/GCodes.h:760-768 that the extracted functions use
	TriggerItem triggers[MaxTriggers];
	TriggerNumbersBitmap triggersPending;
	bool triggerActionRunning;
	std::atomic<bool> feedHoldTriggerActive = false;

	// What the test sets
	bool triggerBusy = false;
	bool reallyPrinting = false;
	bool simulating = false;
	GCodeBuffer triggerGb, autoPauseGb;
};

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return *gcodes; }
	Platform& GetPlatform() noexcept { return platform; }
	PrintMonitor& GetPrintMonitor() noexcept { return printMonitor; }
	Move& GetMove() noexcept { return move; }
	void SendSimpleAlert(MessageType mt, const char *_ecv_array message, const char *_ecv_array title) noexcept
	{
		calls.push_back(std::string("SendSimpleAlert ") + message);
	}

	GCodes *gcodes = nullptr;
	Platform platform;
	PrintMonitor printMonitor;
	Move move;
};

static RepRap reprap;

// ---------------------------------------------------------------------------------------------------------------------------------------
// The real text

#include "StringParser.inc"
#include "TriggerItem.inc"
#include "GCodes.inc"
#include "GCodes3.inc"

// TriggerItem::EvaluateExpression, replaced: each expression has a scripted value, or fails as an unknown variable would
struct ScriptedExpression
{
	bool result = false;
	bool fails = false;
};

static std::map<std::string, ScriptedExpression> expressions;

bool TriggerItem::EvaluateExpression() THROWS(GCodeException)
{
	const auto it = expressions.find(expr.Get().Ptr());
	if (it == expressions.end() || it->second.fails)
	{
		throw GCodeException(nullptr, -1, "unknown value 'sensors.door'");
	}
	return it->second.result;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The checks

static unsigned int passed = 0, failed = 0;
static std::vector<std::string> failures;

static void Expect(bool ok, const std::string& what) noexcept
{
	if (!ok)
	{
		failures.push_back(what);
	}
}

static void Report(const std::string& name) noexcept
{
	if (failures.empty())
	{
		++passed;
		std::printf("PASS  %s\n", name.c_str());
	}
	else
	{
		++failed;
		std::printf("FAIL  %s\n", name.c_str());
		for (const std::string& f : failures)
		{
			std::printf("        %s\n", f.c_str());
		}
	}
	failures.clear();
}

static std::string Join(const std::vector<std::string>& v) noexcept
{
	std::string s = "{";
	for (size_t i = 0; i < v.size(); ++i)
	{
		s += (i == 0) ? " " : ", ";
		for (char c : v[i])
		{
			s += (c == '\n') ? std::string("\\n") : std::string(1, c);		// the messages end in a newline
		}
	}
	return s + " }";
}

static bool Contains(const std::string& s, const char *part) noexcept
{
	return s.find(part) != std::string::npos;
}

// A new machine: all triggers unused, all inputs inactive, nothing printing
static void Fresh() noexcept
{
	delete reprap.gcodes;
	reprap.gcodes = new GCodes();
	reprap.platform = Platform();
	reprap.printMonitor = PrintMonitor();
	reprap.move = Move();
	expressions.clear();
	calls.clear();
	messages.clear();
	now = 1000;
}

static GCodes& G() noexcept { return *reprap.gcodes; }
static TriggerItem& Trigger(unsigned int n) noexcept { return G().triggers[n]; }

struct Outcome
{
	GCodeResult result = GCodeResult::ok;
	std::string reply;
	bool threw = false;

	bool Ok() const noexcept { return !threw && result == GCodeResult::ok; }
	std::string Describe() const noexcept
	{
		return (threw) ? "threw \"" + reply + "\"" : ((result == GCodeResult::ok) ? "ok" : "error") + std::string(", reply \"") + reply + "\"";
	}
};

// Run M581, M581.1 or M582 as GCodes::ConfigureTrigger and GCodes::CheckTrigger get them
static Outcome Exec(const char *_ecv_array line) noexcept
{
	GCodeBuffer gb(line);
	String<GCodeReplyLength> reply;
	Outcome o;
	try
	{
		o.result = (StringStartsWith(line, "M582")) ? G().CheckTrigger(gb, reply.GetRef()) : G().ConfigureTrigger(gb, reply.GetRef());
		o.reply = reply.c_str();
	}
	catch (const GCodeException& e)
	{
		o.threw = true;
		e.GetMessage(reply.GetRef(), &gb);
		o.reply = reply.c_str();
	}
	return o;
}

// Run a command that must succeed
static void Setup(const char *_ecv_array line) noexcept
{
	const Outcome o = Exec(line);
	Expect(o.Ok(), std::string(line) + ": " + o.Describe());
}

static std::string ReportOf(unsigned int n) noexcept
{
	String<StringLength20> line;
	line.printf("M581 T%u", n);
	return Exec(line.c_str()).reply;
}

static void ExpectCalls(const std::vector<std::string>& expected, const char *_ecv_array when) noexcept
{
	Expect(calls == expected, std::string(when) + ": expected " + Join(expected) + ", called " + Join(calls));
}

static const char *_ecv_array ActionName(TriggerAction a) noexcept
{
	static const char *_ecv_array const names[] = { "emergencyStop", "pause", "runMacro", "feedHold", "feedHoldAndAbort" };
	return ((unsigned int)a < NumTriggerActions) ? names[(unsigned int)a] : "?";
}

static void ExpectAction(unsigned int n, TriggerAction expected) noexcept
{
	const TriggerAction a = Trigger(n).GetAction(n);
	Expect(a == expected, "GetAction(" + std::to_string(n) + ") is " + ActionName(a) + ", expected " + ActionName(expected));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 1. A trigger whose action line is M112 makes an emergency stop at once, whatever trigger 0 does

static void TestM112Line(unsigned int k0, bool busy) noexcept
{
	Fresh();
	String<StringLength20> t0;
	t0.printf("M581 T0 P1 S1 K%u", k0);
	Setup(t0.c_str());
	expressions["door"].result = false;
	Setup("M581.1 T5 P\"door\" U\"M112\"");
	G().triggerBusy = busy;
	calls.clear();

	expressions["door"].result = true;
	G().CheckTriggers();
	ExpectCalls({ "DoEmergencyStop" }, "first CheckTriggers after the expression of trigger 5 became true");

	Report("1  trigger 5 U\"M112\" fires, trigger 0 is K" + std::to_string(k0) + ((busy) ? ", trigger channel busy" : "")
			+ ": DoEmergencyStop at once");
}

static void TestM112LineByM582(unsigned int k0) noexcept
{
	Fresh();
	String<StringLength20> t0;
	t0.printf("M581 T0 P1 S1 K%u", k0);
	Setup(t0.c_str());
	expressions["door"].result = false;
	Setup("M581.1 T5 P\"door\" U\"M112\"");
	calls.clear();

	Setup("M582 T5 S1");
	G().CheckTriggers();
	ExpectCalls({ "DoEmergencyStop" }, "CheckTriggers after M582 T5 S1");

	Report("1  M582 T5 S1 of trigger 5 U\"M112\", trigger 0 is K" + std::to_string(k0) + ": DoEmergencyStop at once");
}

// 2. The lock refuses K

static void TestLockRefusesK() noexcept
{
	Fresh();
	Setup("M581 T0 P1 S1");											// trigger 0 without K is an emergency stop, so it is locked
	const Outcome o = Exec("M581 T0 K2");
	Expect(!o.Ok() && Contains(o.reply, "locked"), "M581 T0 K2 on the locked trigger 0: " + o.Describe());
	ExpectAction(0, TriggerAction::emergencyStop);
	Report("2  M581 T0 K2 is refused while trigger 0 is locked, it stays an emergency stop");

	Fresh();
	expressions["x"].result = false;
	Setup("M581.1 T5 P\"x\" U\"M42 P0 S1\"");						// a trigger with an action line is locked
	for (const char *_ecv_array cmd : { "M581.1 T5 K3", "M581 T5 K3" })
	{
		const Outcome o2 = Exec(cmd);
		Expect(!o2.Ok() && Contains(o2.reply, "locked"), std::string(cmd) + " on the locked trigger 5: " + o2.Describe());
	}
	ExpectAction(5, TriggerAction::runMacro);
	Report("2  M581.1 T5 K3 and M581 T5 K3 are refused while trigger 5 with an action line is locked");
}

// 3. A K-only command that makes a trigger an emergency stop locks it

static void TestLockAfterKOnly() noexcept
{
	Fresh();
	Setup("M581 T5 P2 S1");
	Expect(!Contains(ReportOf(5), "locked"), "M581 T5 P2 S1 locked the trigger: " + ReportOf(5));
	Setup("M581 T5 K0");
	ExpectAction(5, TriggerAction::emergencyStop);
	const Outcome o = Exec("M581 T5 P-1");
	Expect(!o.Ok() && Contains(o.reply, "locked"), "M581 T5 P-1 after M581 T5 K0: " + o.Describe());
	Expect(!Trigger(5).IsUnused(), "trigger 5 was deleted");
	Report("3  M581 T5 K0 on the unlocked trigger 5 (P2 S1) locks it: M581 T5 P-1 is refused");
}

// K is read before the rest of the command, as upstream does, so a command that fails later still leaves the new K. The lock follows it.
static void TestLockAfterFailedCommand() noexcept
{
	Fresh();
	Setup("M581 T5 P2 S1");
	const Outcome o = Exec("M581.1 T5 K0 U\"M42 P0 S1\"");
	Expect(!o.Ok() && Contains(o.reply, "does not run a macro file"), "M581.1 T5 K0 U\"M42 P0 S1\": " + o.Describe());
	ExpectAction(5, TriggerAction::emergencyStop);
	Expect(Contains(ReportOf(5), "locked until restart"), "trigger 5 is not locked: " + ReportOf(5));
	const Outcome d = Exec("M581 T5 P-1");
	Expect(!d.Ok() && Contains(d.reply, "locked"), "M581 T5 P-1: " + d.Describe());
	Expect(!Trigger(5).IsUnused(), "trigger 5 was deleted");
	Report("3  M581.1 T5 K0 U\"M42 P0 S1\" on trigger 5 (P2 S1) is refused, but K0 stays and locks it: M581 T5 P-1 is refused");

	Fresh();
	Setup("M581 T5 P2 S1");
	const Outcome o2 = Exec("M581.1 T5 K0 D70000");
	Expect(o2.threw, "M581.1 T5 K0 D70000: " + o2.Describe());
	ExpectAction(5, TriggerAction::emergencyStop);
	const Outcome d2 = Exec("M581 T5 P-1");
	Expect(!d2.Ok() && Contains(d2.reply, "locked"), "M581 T5 P-1: " + d2.Describe());
	Expect(!Trigger(5).IsUnused(), "trigger 5 was deleted");
	Report("3  M581.1 T5 K0 D70000 on trigger 5 (P2 S1) fails, but K0 stays and locks it: M581 T5 P-1 is refused");
}

// 4. An action line only on a trigger that runs a macro file

static void TestActionLineNeedsMacroTrigger() noexcept
{
	Fresh();
	expressions["x"].result = false;
	const Outcome o = Exec("M581.1 T5 K3 P\"x\" U\"M42 P0 S1\"");
	Expect(!o.Ok() && Contains(o.reply, "does not run a macro file"), "M581.1 T5 K3 P\"x\" U\"M42 P0 S1\": " + o.Describe());
	Expect(Trigger(5).IsUnused(), "trigger 5 was configured");
	Expect(!Trigger(5).HasActionLine(), "trigger 5 has an action line");
	Report("4  M581.1 T5 K3 P\"x\" U\"M42 P0 S1\" is refused: a feed hold trigger does not run a macro file");
}

static void TestTrigger0K2ActionLine() noexcept
{
	Fresh();
	expressions["x"].result = false;
	Setup("M581.1 T0 K2 P\"x\" U\"M42 P0 S1\"");
	Expect(Trigger(0).HasActionLine(), "trigger 0 has no action line");
	ExpectAction(0, TriggerAction::runMacro);

	calls.clear();
	G().triggerBusy = true;
	expressions["x"].result = true;
	G().CheckTriggers();
	ExpectCalls({ }, "CheckTriggers while the trigger channel is busy");
	G().triggerBusy = false;
	G().CheckTriggers();
	ExpectCalls({ "PutAndDecode M42 P0 S1" }, "CheckTriggers once the trigger channel is free");
	Report("4  M581.1 T0 K2 P\"x\" U\"M42 P0 S1\" is accepted; the line runs through the trigger channel when it is free");
}

// An action line given before a hard-wired K: the K deletes it, as M581 on inputs or endstops does
static void TestHardWiredKDeletesActionLine(TriggerAction k) noexcept
{
	Fresh();
	expressions["door"].result = false;
	Setup("M581.1 T5 U\"M112\"");										// trigger 5 is unused, so it is not locked
	String<StringLength20> cmd;
	cmd.printf("M581.1 T5 K%u P\"door\"", (unsigned int)k);
	Setup(cmd.c_str());
	Expect(!Trigger(5).HasActionLine(), "trigger 5 still has its action line");
	const std::string report = ReportOf(5);
	Expect(!Contains(report, ", action "), "the report shows an action: " + report);
	ExpectAction(5, k);

	G().reallyPrinting = true;
	calls.clear();
	expressions["door"].result = true;
	G().CheckTriggers();
	const std::vector<std::string> expected = (k == TriggerAction::pause) ? std::vector<std::string>{ "DoAsynchronousPause", "SendSimpleAlert Print paused by external trigger" }
												: (k == TriggerAction::feedHold) ? std::vector<std::string>{ "FeedHold" }
													: std::vector<std::string>{ "StopAndAbortMotion" };
	ExpectCalls(expected, "first CheckTriggers after the expression of trigger 5 became true, printing");
	Report(std::string("4  M581.1 T5 U\"M112\", then M581.1 T5 ") + (cmd.c_str() + 10) + ": the action line is gone, firing calls "
			+ expected[0] + ", not DoEmergencyStop");
}

// 5. The fail-safe follows the action, not the trigger number

static void TestFailSafeOnCheck() noexcept
{
	Fresh();
	expressions["probe"].result = false;
	Setup("M581.1 T5 K0 P\"probe\"");
	expressions["probe"].fails = true;
	messages.clear();
	Expect(Trigger(5).Check(5), "Check(5) returned false");
	const std::string report5 = ReportOf(5);
	Expect(Contains(report5, " fires when expression {probe}"), "trigger 5 is no longer configured and enabled: " + report5);
	Expect(messages.size() == 1 && Contains(messages[0], "Trigger 5 fired"), "messages " + Join(messages));
	Report("5  K0 expression trigger 5 that cannot be evaluated fires and stays configured");

	Fresh();
	expressions["probe"].result = false;
	Setup("M581.1 T0 K2 P\"probe\"");
	expressions["probe"].fails = true;
	messages.clear();
	Expect(!Trigger(0).Check(0), "Check(0) returned true");
	const std::string report0 = ReportOf(0);
	Expect(Contains(report0, "if enabled would fire"), "trigger 0 is not disabled: " + report0);
	Expect(messages.size() == 1 && Contains(messages[0], "Trigger 0 disabled"), "messages " + Join(messages));
	Report("5  K2 expression trigger 0 that cannot be evaluated is disabled and does not fire");
}

static void TestFailSafeOnConfigure() noexcept
{
	Fresh();
	expressions["probe"].fails = true;
	const Outcome o5 = Exec("M581.1 T5 K0 P\"probe\"");
	Expect(o5.threw, "M581.1 T5 K0 P\"probe\": " + o5.Describe());
	Expect(!Trigger(5).IsUnused(), "trigger 5 was cleared");
	Expect(Trigger(5).Check(5), "Check(5) returned false");
	Report("5  M581.1 T5 K0 with an expression that cannot be evaluated keeps trigger 5, and its next check fires it");

	Fresh();
	expressions["probe"].fails = true;
	const Outcome o0 = Exec("M581.1 T0 K2 P\"probe\"");
	Expect(o0.threw, "M581.1 T0 K2 P\"probe\": " + o0.Describe());
	Expect(Trigger(0).IsUnused(), "trigger 0 was kept: " + ReportOf(0));
	Report("5  M581.1 T0 K2 with an expression that cannot be evaluated clears trigger 0");

	// The kept emergency stop trigger is locked
	struct { unsigned int n; const char *_ecv_array cmd; } const keptTriggers[] = { { 5, "M581.1 T5 K0 P\"probe\"" }, { 0, "M581.1 T0 P\"probe\"" } };
	for (const auto& kt : keptTriggers)
	{
		Fresh();
		expressions["probe"].fails = true;
		const Outcome o = Exec(kt.cmd);
		Expect(o.threw, std::string(kt.cmd) + ": " + o.Describe());
		Expect(!Trigger(kt.n).IsUnused(), "trigger " + std::to_string(kt.n) + " was cleared");
		const std::string report = ReportOf(kt.n);
		Expect(Contains(report, "locked until restart"), "trigger " + std::to_string(kt.n) + " is not locked: " + report);
		String<StringLength20> del;
		del.printf("M581 T%u P-1", kt.n);
		const Outcome d = Exec(del.c_str());
		Expect(!d.Ok() && Contains(d.reply, "locked"), std::string(del.c_str()) + ": " + d.Describe());
		Expect(!Trigger(kt.n).IsUnused(), "trigger " + std::to_string(kt.n) + " was deleted");
		Report(std::string("5  ") + kt.cmd + " with an expression that cannot be evaluated: the kept trigger is locked, "
				+ del.c_str() + " is refused");
	}
}

// 6. What upstream does

static void TestFeedHold() noexcept
{
	Fresh();
	Setup("M581 T4 P2 S1 K3");
	calls.clear();
	reprap.platform.ports[2].active = true;
	G().CheckTriggers();
	ExpectCalls({ "FeedHold" }, "CheckTriggers after input 2 became active");
	Expect(G().feedHoldTriggerActive, "feedHoldTriggerActive is false while input 2 is active");
	G().CheckTriggers();
	ExpectCalls({ "FeedHold" }, "second CheckTriggers with input 2 still active");
	Expect(G().feedHoldTriggerActive, "feedHoldTriggerActive is false while input 2 is still active");
	reprap.platform.ports[2].active = false;
	G().CheckTriggers();
	ExpectCalls({ "FeedHold" }, "CheckTriggers after input 2 became inactive");
	Expect(!G().feedHoldTriggerActive, "feedHoldTriggerActive is true after input 2 became inactive");
	Report("6  K3: Move::FeedHold, feedHoldTriggerActive while the input is active");

	Fresh();
	Setup("M581 T4 P2 S1 K3");
	G().simulating = true;
	calls.clear();
	reprap.platform.ports[2].active = true;
	G().CheckTriggers();
	ExpectCalls({ }, "CheckTriggers while simulating");
	Report("6  K3 while simulating: no feed hold");
}

static void TestFeedHoldAndAbort() noexcept
{
	Fresh();
	Setup("M581 T6 P3 S1 K4");
	calls.clear();
	reprap.platform.ports[3].active = true;
	G().CheckTriggers();
	ExpectCalls({ "StopAndAbortMotion" }, "CheckTriggers after input 3 became active");
	Report("6  K4: StopAndAbortMotion for the autopause channel");
}

static void TestPause() noexcept
{
	Fresh();
	Setup("M581 T1 P4 S1");
	G().reallyPrinting = true;
	calls.clear();
	reprap.platform.ports[4].active = true;
	G().CheckTriggers();
	ExpectCalls({ "DoAsynchronousPause", "SendSimpleAlert Print paused by external trigger" }, "CheckTriggers while printing");
	Report("6  trigger 1 without K pauses when printing");

	for (bool held : { false, true })
	{
		Fresh();
		Setup("M581 T1 P4 S1");
		G().reallyPrinting = held;
		reprap.move.held = held;
		calls.clear();
		reprap.platform.ports[4].active = true;
		G().CheckTriggers();
		ExpectCalls({ }, (held) ? "CheckTriggers while held" : "CheckTriggers while not printing");
		G().reallyPrinting = true;
		reprap.move.held = false;
		G().CheckTriggers();
		ExpectCalls({ }, "CheckTriggers once printing and not held: the trigger was dropped");
		Report((held) ? "6  trigger 1 is ignored while a feed hold is active" : "6  trigger 1 is ignored when not printing");
	}
}

static void TestRunMacro() noexcept
{
	Fresh();
	Setup("M581 T3 P5 S1 K2");
	calls.clear();
	reprap.platform.ports[5].active = true;
	G().CheckTriggers();
	ExpectCalls({ "DoFileMacro trigger3.g" }, "CheckTriggers after input 5 became active");
	Report("6  K2 trigger 3: DoFileMacro(trigger3.g) on the trigger channel");

	Fresh();
	Setup("M581 T0 P1 S1");
	calls.clear();
	reprap.platform.ports[1].active = true;
	G().CheckTriggers();
	ExpectCalls({ "DoEmergencyStop" }, "CheckTriggers after input 1 became active");
	Report("6  trigger 0 without K: DoEmergencyStop");
}

static void TestImmediateOrder() noexcept
{
	Fresh();
	Setup("M581 T2 P6 S1 K3");
	Setup("M581 T3 P7 S1 K4");
	calls.clear();
	reprap.platform.ports[6].active = true;
	reprap.platform.ports[7].active = true;
	G().CheckTriggers();
	ExpectCalls({ "StopAndAbortMotion", "FeedHold" }, "CheckTriggers after inputs 6 and 7 became active");
	Report("6  K4 trigger 3 before K3 trigger 2 in the same pass");

	Fresh();
	Setup("M581 T2 P6 S1 K3");
	Setup("M581 T3 P7 S1 K4");
	Setup("M581 T9 P8 S1 K0");
	calls.clear();
	reprap.platform.ports[6].active = true;
	reprap.platform.ports[7].active = true;
	reprap.platform.ports[8].active = true;
	G().CheckTriggers();
	ExpectCalls({ "DoEmergencyStop" }, "CheckTriggers after inputs 6, 7 and 8 became active");
	Report("6  K0 trigger 9 before K4 trigger 3 and K3 trigger 2: only the emergency stop");
}

static void TestReport() noexcept
{
	Fresh();
	const Outcome o = Exec("M581 T2");
	Expect(o.Ok() && StringStartsWith(o.reply.c_str(), "Trigger 2 (K2, run macro) "), "M581 T2: " + o.Describe());
	Report("6  M581 T2 reports \"Trigger 2 (K2, run macro) ...\"");
}

// ---------------------------------------------------------------------------------------------------------------------------------------

int main()
{
	for (unsigned int k0 : { 3u, 2u })
	{
		for (bool busy : { false, true })
		{
			TestM112Line(k0, busy);
		}
		TestM112LineByM582(k0);
	}

	TestLockRefusesK();
	TestLockAfterKOnly();
	TestLockAfterFailedCommand();

	TestActionLineNeedsMacroTrigger();
	TestTrigger0K2ActionLine();
	for (TriggerAction k : { TriggerAction::pause, TriggerAction::feedHold, TriggerAction::feedHoldAndAbort })
	{
		TestHardWiredKDeletesActionLine(k);
	}

	TestFailSafeOnCheck();
	TestFailSafeOnConfigure();

	TestFeedHold();
	TestFeedHoldAndAbort();
	TestPause();
	TestRunMacro();
	TestImmediateOrder();
	TestReport();

	delete reprap.gcodes;
	std::printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
