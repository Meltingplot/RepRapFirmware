// Host test of M24 during a feed hold (upstream 2092f4148, "M24 during a feed hold no longer resumes a paused job"), compiled from the real
// text that the Makefile extracts: RegularGCodeInput::CheckForUrgentCommand (src/GCodes/GCodeInput.cpp) with enum class GCodeInputState
// (src/GCodes/GCodeInput.h), and the feed hold block of "case 24:" in GCodes::HandleMcode (src/GCodes/GCodes2.cpp).
//
// M24 while a feed hold is active releases the hold. Before 2092f4148 it also resumed a print that was paused, because the hold block of
// HandleMcode only stopped when no print was paused ("If the print is paused as well, carry on and resume it too") and the urgent command
// scanner left an M24 alone while a print was paused. Since 2092f4148 the hold block always stops after the release ("A paused print stays
// paused, because the hold stopped a jog or a macro and not the print"), and the scanner releases the hold and swallows a plain M24 whether
// or not a print is paused. A paused print needs an M24 of its own once the hold is released.
//
// The test plays the parts of Move, GCodes and RepRap that this text uses, with counters for what it calls, and checks both parts in the
// cases that 2092f4148 changes (hold active, print paused) and in those it leaves as they were (no print paused, a feed hold trigger still
// active, no hold, M24 with a parameter).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// src/Platform/MessageType.h:14, only the type matters here
enum MessageType : uint32_t { UsbMessage = 1 };

// src/GCodes/GCodes.h:70-78
enum class PauseState : uint8_t { notPaused = 0, pausing, paused, resuming, cancelling };

// CANlib src/GCodeResult.h:21, the values the hold block uses
enum class GCodeResult : uint8_t { notFinished, ok, warning, error };

#include "GCodeInputState.inc"

// What the hold block uses of the reply StringRef
struct Reply
{
	void copy(const char *s) noexcept { text = s; }
	std::string text;
};

// The members of Move that the text calls (src/Movement/Move.h:398-400, Move::FeedHold and Move::ReleaseFeedHold in src/Movement/Move.cpp)
class Move
{
public:
	bool IsFeedHoldActive() const noexcept { return held; }
	void FeedHold() noexcept { ++feedHolds; held = true; }
	void ReleaseFeedHold() noexcept { ++releases; held = false; }

	bool held = false;
	unsigned int feedHolds = 0, releases = 0;
};

// The members of GCodes that the text uses (src/GCodes/GCodes.h:153-156, 507, 681, 768). RunM24HoldBlock wraps the hold block of
// "case 24:" in a switch, as HandleMcode has it, so that its "break" leaves M24 and falling off its end carries on with the pause and
// resume code that follows it in HandleMcode (src/GCodes/GCodes2.cpp, "if (pauseState == PauseState::pausing ...", which resumes a
// paused print)
class GCodes
{
public:
	enum class M24Outcome { done, carriesOn };

	PauseState GetPauseState() const noexcept { return pauseState; }
	bool IsFeedHoldTriggerActive() const noexcept { return feedHoldTriggerActive; }
	void RequestStopAndAbort() noexcept { ++stopAndAborts; }
	void CancelWaitForTemperatures(bool) noexcept { }
	void Reset() noexcept { }
	void RestoreReadAheadPositions() noexcept { ++readAheadRestores; }

	M24Outcome RunM24HoldBlock(Reply& reply, GCodeResult& result) noexcept;

	PauseState pauseState = PauseState::notPaused;
	std::atomic<bool> feedHoldTriggerActive = false;
	unsigned int stopAndAborts = 0, readAheadRestores = 0;
};

class RepRap
{
public:
	Move& GetMove() noexcept { return move; }
	GCodes& GetGCodes() noexcept { return gcodes; }
	void EmergencyStop() noexcept { }
	void DeferredDiagnostics(MessageType) noexcept { }

	Move move;
	GCodes gcodes;
};

static RepRap reprap;

GCodes::M24Outcome GCodes::RunM24HoldBlock(Reply& reply, GCodeResult& result) noexcept
{
	const int code = 24;
	switch (code)
	{
	case 24:
#include "M24HoldBlock.inc"
		return M24Outcome::carriesOn;
	}
	return M24Outcome::done;
}

// The members of RegularGCodeInput (src/GCodes/GCodeInput.h:86-107) that CheckForUrgentCommand uses. Reset counts its calls; the real one
// also empties the buffer (src/GCodes/GCodeInput.cpp:92-96), which is how the scanner swallows a command it has handled
class RegularGCodeInput
{
public:
	bool CheckForUrgentCommand(char c) noexcept;
	void Reset() noexcept { state = GCodeInputState::idle; ++resets; }

	MessageType mtype = UsbMessage;
	GCodeInputState state = GCodeInputState::idle;
	bool writingFile = false;
	unsigned int resets = 0;
};

#include "CheckForUrgentCommand.inc"

static unsigned int passed = 0, failed = 0;

static void Add(std::string& why, const std::string& what) noexcept
{
	why += (why.empty() ? "" : "; ") + what;
}

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
		printf("FAIL %s: %s\n", name, why.c_str());
	}
}

static void SetUp(bool held, PauseState pauseState, bool triggerActive) noexcept
{
	reprap.move.held = held;
	reprap.move.feedHolds = reprap.move.releases = 0;
	reprap.gcodes.pauseState = pauseState;
	reprap.gcodes.feedHoldTriggerActive = triggerActive;
	reprap.gcodes.stopAndAborts = reprap.gcodes.readAheadRestores = 0;
}

// Feed a line to the scanner as an input task does (GCodeInput.cpp:390 and 663) and say whether it swallowed the line
static bool Scan(const char *line) noexcept
{
	RegularGCodeInput input;
	bool swallowed = false;
	for (const char *p = line; *p != 0 && !swallowed; ++p)
	{
		swallowed = input.CheckForUrgentCommand(*p);
	}
	if (swallowed && input.resets != 1)
	{
		printf("note: the scanner reset its input %u times\n", input.resets);
	}
	return swallowed;
}

static std::string ExpectScan(const char *line, bool swallow, unsigned int releases) noexcept
{
	std::string why;
	const bool heldBefore = reprap.move.held;
	const bool swallowed = Scan(line);
	if (swallowed != swallow)
	{
		Add(why, swallow ? "the scanner did not swallow the line, so it is queued behind the held code" : "the scanner swallowed the line");
	}
	if (reprap.move.releases != releases)
	{
		Add(why, "the scanner released the hold " + std::to_string(reprap.move.releases) + " times instead of " + std::to_string(releases));
	}
	if (reprap.move.held != (heldBefore && releases == 0))
	{
		Add(why, reprap.move.held ? "the hold is still active" : "the hold was released");
	}
	return why;
}

int main()
{
	// The urgent command scanner of the serial, USB and network inputs (RegularGCodeInput, src/GCodes/GCodeInput.cpp:158-168)
	SetUp(true, PauseState::paused, false);
	Report("scanner: a plain M24 during a hold with a paused print releases the hold and is swallowed", ExpectScan("M24\n", true, 1));

	SetUp(true, PauseState::paused, false);
	Report("scanner: M24 with a comment during a hold with a paused print releases the hold and is swallowed", ExpectScan("M24;go on\n", true, 1));

	SetUp(true, PauseState::notPaused, false);
	Report("scanner: a plain M24 during a hold without a paused print releases the hold and is swallowed", ExpectScan("M24\n", true, 1));

	SetUp(true, PauseState::paused, true);
	Report("scanner: a plain M24 during a hold whose trigger is still active is left to HandleMcode", ExpectScan("M24\n", false, 0));

	SetUp(false, PauseState::paused, false);
	Report("scanner: a plain M24 without a hold is left to HandleMcode", ExpectScan("M24\n", false, 0));

	SetUp(true, PauseState::paused, false);
	Report("scanner: M24 with a parameter during a hold is left to HandleMcode", ExpectScan("M24 P0\n", false, 0));

	// The feed hold block of M24 in GCodes::HandleMcode (src/GCodes/GCodes2.cpp:1228-1243), which every M24 reaches that the scanner left
	// alone, including all M24 from the SBC
	{
		SetUp(true, PauseState::paused, false);
		Reply reply;
		GCodeResult result = GCodeResult::ok;
		std::string why;
		if (reprap.gcodes.RunM24HoldBlock(reply, result) != GCodes::M24Outcome::done)
		{
			Add(why, "M24 carried on to the pause and resume code, which resumes the paused print");
		}
		if (reprap.move.releases != 1) { Add(why, "released the hold " + std::to_string(reprap.move.releases) + " times instead of once"); }
		if (reprap.gcodes.readAheadRestores != 1) { Add(why, "did not give the held streams their read-ahead positions back"); }
		if (reprap.gcodes.pauseState != PauseState::paused) { Add(why, "the pause state changed"); }
		if (result != GCodeResult::ok || !reply.text.empty()) { Add(why, "replied \"" + reply.text + "\""); }
		Report("HandleMcode: M24 during a hold with a paused print releases the hold and keeps the print paused", why);
	}

	{
		SetUp(true, PauseState::notPaused, false);
		Reply reply;
		GCodeResult result = GCodeResult::ok;
		std::string why;
		if (reprap.gcodes.RunM24HoldBlock(reply, result) != GCodes::M24Outcome::done) { Add(why, "M24 carried on to the pause and resume code"); }
		if (reprap.move.releases != 1) { Add(why, "released the hold " + std::to_string(reprap.move.releases) + " times instead of once"); }
		if (reprap.gcodes.readAheadRestores != 1) { Add(why, "did not give the held streams their read-ahead positions back"); }
		if (result != GCodeResult::ok) { Add(why, "replied \"" + reply.text + "\""); }
		Report("HandleMcode: M24 during a hold without a paused print releases the hold and stops there", why);
	}

	{
		SetUp(true, PauseState::paused, true);
		Reply reply;
		GCodeResult result = GCodeResult::ok;
		std::string why;
		if (reprap.gcodes.RunM24HoldBlock(reply, result) != GCodes::M24Outcome::done) { Add(why, "M24 carried on to the pause and resume code"); }
		if (reprap.move.releases != 0) { Add(why, "released the hold"); }
		if (result != GCodeResult::error || reply.text.find("feed hold trigger") == std::string::npos)
		{
			Add(why, "did not refuse with the trigger message, replied \"" + reply.text + "\"");
		}
		Report("HandleMcode: M24 during a hold whose trigger is still active is refused and releases nothing", why);
	}

	{
		SetUp(false, PauseState::paused, false);
		Reply reply;
		GCodeResult result = GCodeResult::ok;
		std::string why;
		if (reprap.gcodes.RunM24HoldBlock(reply, result) != GCodes::M24Outcome::carriesOn)
		{
			Add(why, "M24 stopped before the pause and resume code, so a paused print is not resumed");
		}
		if (reprap.move.releases != 0) { Add(why, "released a hold that was not active"); }
		Report("HandleMcode: M24 without a hold goes on to the pause and resume code, which resumes the paused print", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
