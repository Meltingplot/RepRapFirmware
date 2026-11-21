// Host test of Move::OnEndstopOrZProbeStatesChanged (src/Movement/Move.cpp), compiled from the real function text that the
// Makefile extracts, together with the real DMState and MovementFlags.
//
// The CAN receiver task calls it after an expansion board reports an endstop, Z probe or GpIn change (src/CAN/CommandProcessor.cpp),
// and the main task calls it after a filament monitor's presence or motion changes (src/FilamentMonitors/FilamentMonitor.cpp).
// It then lets Move::CheckEndstops act on the active endstop list of EndstopsManager. That list keeps the endstops and Z probes of
// a homing or probing move that ended without a trigger, because only a trigger takes an entry out (EndstopsManager::CheckEndstops).
// The step ISR checks the endstops only while a DM whose step is due executes a segment with the checkEndstops flag
// (Move::StepDrivers), the phase stepping control loop only while a phase stepping DM in a motion state does
// (Move::PhaseStepControlLoop). This test checks that an input change does not let a stale entry stop a move that does not check
// endstops, and that it still stops a homing or probing move, including one executed entirely by remote drivers.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// As for the Duet 3 MB6HC (src/Config/Pins_Duet3_MB6HC.h)
#define SUPPORT_CAN_EXPANSION	1
#define SUPPORT_PHASE_STEPPING	1
#define SUPPORT_CLOSED_LOOP		0

#define _ecv_null

#include "DMState.inc"
#include "MovementFlags.inc"

// Just what OnEndstopOrZProbeStatesChanged uses, and what the test needs to describe a DM. In the firmware these members are
// private and Move is a friend of DriveMovement.
class DriveMovement
{
public:
	DMState state = DMState::idle;
	MovementFlags segmentFlags;
	DriveMovement *_ecv_null nextDM = nullptr;
	uint32_t driversNormallyUsed = 0;								// 0 if the axis or extruder has no local drivers

	DriveMovement() noexcept { segmentFlags.Init(); }				// as DriveMovement::Init
};

constexpr uint32_t NvicPriorityStep = 5;							// value for the SAME5x in src/RepRapFirmware.h; not used by the test

static unsigned int boostDepth = 0;

// While this is alive the step interrupt and, because the FreeRTOS kernel interrupts have the lowest priority, all other tasks
// are shut out, so nothing changes the DM lists or the active endstop list
class BasePriorityBooster
{
public:
	explicit BasePriorityBooster(uint32_t) noexcept { ++boostDepth; }
	~BasePriorityBooster() noexcept { --boostDepth; }
};

static unsigned int wakeCalls = 0;
static bool wokeWhileBoosted = false;

namespace CanInterface
{
	void WakeAsyncSender() noexcept
	{
		++wakeCalls;
		if (boostDepth != 0) { wokeWhileBoosted = true; }
	}
}

class Move
{
public:
	void OnEndstopOrZProbeStatesChanged() noexcept;
	bool CheckEndstops(bool executingMove) noexcept;				// played below

	DriveMovement *_ecv_null activeDMs = nullptr;
	DriveMovement *_ecv_null phaseStepDMs = nullptr;
};

#include "OnEndstopOrZProbeStatesChanged.inc"

// The active endstop list of EndstopsManager, reduced to one entry
static bool entryActive = false;									// an endstop or Z probe is in the active list
static bool entryTriggered = false;									// and reads triggered

static unsigned int checkCalls = 0;
static bool checkedUnboosted = false;
static bool checkedNotExecuting = false;
static unsigned int drivesStopped = 0;
static unsigned int hitsRecorded = 0;

// Plays Move::CheckEndstops with EndstopsManager::CheckEndstops: an entry that reads triggered gives EndstopHitAction::stopAll, which
// takes it out of the list (the list is cleared), StopAllDrivers stops every drive that is moving (StopLogicalDrive takes its DM out
// of its list and makes it idle), and the hit is recorded (GCodes::MoveStoppedByZProbe or GCodes::RecordEndstopTriggered), whether
// or not anything was moving. The result is true if remote drivers were stopped, so that the async sender must be woken.
bool Move::CheckEndstops(bool executingMove) noexcept
{
	++checkCalls;
	if (boostDepth == 0) { checkedUnboosted = true; }
	if (!executingMove) { checkedNotExecuting = true; }
	if (!entryActive || !entryTriggered)
	{
		return false;
	}

	entryActive = false;
	++hitsRecorded;
	bool wakeAsyncSender = false;
	for (DriveMovement *_ecv_null *list : { &activeDMs, &phaseStepDMs })
	{
		for (DriveMovement *_ecv_null dm = *list; dm != nullptr; )
		{
			DriveMovement *_ecv_null const next = dm->nextDM;
			if (dm->state != DMState::idle)
			{
				++drivesStopped;
				if (dm->driversNormallyUsed == 0) { wakeAsyncSender = true; }
				dm->state = DMState::idle;
			}
			dm->nextDM = nullptr;
			dm = next;
		}
		*list = nullptr;
	}
	return wakeAsyncSender;
}

// A DM as NewSegment leaves it (src/Movement/DriveMovement.cpp): it copies the segment flags before it decides between 'starting'
// (segment not due yet) and a motion state
static DriveMovement MakeDm(DMState state, bool checkEndstops, bool isExtruder, uint32_t localDrivers) noexcept
{
	DriveMovement dm;
	dm.state = state;
	dm.segmentFlags.Clear();
	dm.segmentFlags.nonPrintingMove = checkEndstops;				// homing and probing moves are not printing moves
	dm.segmentFlags.checkEndstops = checkEndstops;
	dm.segmentFlags.isExtruder = isExtruder;
	dm.segmentFlags.executing = (state > DMState::starting);
	dm.driversNormallyUsed = localDrivers;
	return dm;
}

static void Link(DriveMovement *_ecv_null &list, std::vector<DriveMovement>& dms) noexcept
{
	list = nullptr;
	for (size_t i = dms.size(); i != 0; --i)
	{
		dms[i - 1].nextDM = list;
		list = &dms[i - 1];
	}
}

struct TestCase
{
	const char *name;
	std::vector<DriveMovement> active;
	std::vector<DriveMovement> phaseStepping;
	bool expectStop;												// the input change must stop the move and record the hit
};

static unsigned int passed = 0, failed = 0;

static void Run(TestCase tc) noexcept
{
	Move move;
	Link(move.activeDMs, tc.active);
	Link(move.phaseStepDMs, tc.phaseStepping);

	entryActive = entryTriggered = true;							// a stale or current endstop or Z probe that reads triggered
	checkCalls = drivesStopped = hitsRecorded = wakeCalls = 0;
	checkedUnboosted = checkedNotExecuting = wokeWhileBoosted = false;
	bool remoteMoving = false;
	for (const std::vector<DriveMovement> *v : { &tc.active, &tc.phaseStepping })
	{
		for (const DriveMovement& dm : *v)
		{
			if (dm.state != DMState::idle && dm.driversNormallyUsed == 0) { remoteMoving = true; }
		}
	}

	move.OnEndstopOrZProbeStatesChanged();

	std::vector<std::string> reasons;
	const bool stopped = (drivesStopped != 0);
	if (tc.expectStop)
	{
		if (!stopped || hitsRecorded != 1)
		{
			reasons.push_back("the move was not stopped although it checks endstops (" + std::to_string(drivesStopped) + " drive(s) stopped, "
								+ std::to_string(hitsRecorded) + " hit(s) recorded)");
		}
	}
	else if (stopped || hitsRecorded != 0)
	{
		reasons.push_back("an endstop that no executing move checks acted (CheckEndstops called " + std::to_string(checkCalls) + " time(s), "
							+ std::to_string(drivesStopped) + " drive(s) stopped, " + std::to_string(hitsRecorded) + " hit(s) recorded)");
	}
	if (checkedUnboosted) { reasons.push_back("CheckEndstops was called with the step interrupt enabled"); }
	if (checkedNotExecuting) { reasons.push_back("CheckEndstops was called with executingMove false"); }
	const unsigned int expectedWakes = (stopped && remoteMoving) ? 1 : 0;
	if (wakeCalls != expectedWakes)
	{
		reasons.push_back("WakeAsyncSender called " + std::to_string(wakeCalls) + " time(s), expected " + std::to_string(expectedWakes));
	}
	if (wokeWhileBoosted) { reasons.push_back("WakeAsyncSender was called with the step interrupt shut out"); }
	if (boostDepth != 0) { reasons.push_back("the base priority was not restored"); }

	if (reasons.empty())
	{
		++passed;
		printf("PASS %s\n", tc.name);
	}
	else
	{
		++failed;
		printf("FAIL %s:", tc.name);
		for (const std::string& r : reasons) { printf(" %s;", r.c_str()); }
		printf("\n");
	}
}

int main() noexcept
{
	constexpr uint32_t Local = 1;									// some local driver
	constexpr uint32_t Remote = 0;									// no local drivers, all on a CAN expansion or tool board

	// Moves that do not check endstops, with an endstop or Z probe left in the active list by an earlier homing or probing move
	// (G30, G38.x or G1 H1/H3/H4 that ended without a trigger) that now reads triggered
	Run({ "no move executing, stale triggered entry in the active list", {}, {}, false });
	Run({ "print move on local drivers, stale triggered entry",
			{ MakeDm(DMState::cartLinear, false, false, Local), MakeDm(DMState::cartAccel, false, false, Local), MakeDm(DMState::cartLinear, false, true, Local) },
			{}, false });
	Run({ "print move with an extruder on a tool board (remote position updates, state ending), stale triggered entry",
			{ MakeDm(DMState::cartDecelNoReverse, false, false, Local), MakeDm(DMState::ending, false, true, Remote) },
			{}, false });
	Run({ "print move with phase stepping axes, stale triggered entry",
			{ MakeDm(DMState::cartLinear, false, true, Local) },
			{ MakeDm(DMState::phaseStepping, false, false, Local), MakeDm(DMState::phaseStepping, false, false, Local) }, false });

	// Moves that check endstops and whose endstop, Z probe or filament endstop now reads triggered
	Run({ "local homing move (G1 H1 X) executing",
			{ MakeDm(DMState::cartAccel, true, false, Local) }, {}, true });
	Run({ "homing move executed entirely by remote drivers (G1 H1 Z, drivers on an expansion board)",
			{ MakeDm(DMState::cartLinear, true, false, Remote) }, {}, true });
	Run({ "G1 H1 E move executed by a tool board extruder, filament endstop changed",
			{ MakeDm(DMState::cartDecelNoReverse, true, true, Remote) }, {}, true });
	Run({ "probing move with local and remote Z drivers executing",
			{ MakeDm(DMState::cartLinear, true, false, Local), MakeDm(DMState::cartLinear, true, false, Remote) }, {}, true });
	Run({ "DM that checks endstops behind DMs that do not in the list",
			{ MakeDm(DMState::cartLinear, false, false, Local), MakeDm(DMState::starting, true, false, Local), MakeDm(DMState::cartLinear, true, false, Remote) },
			{}, true });
	Run({ "phase stepped homing move executing",
			{}, { MakeDm(DMState::phaseStepping, true, false, Local) }, true });
	Run({ "DM in the phase stepping list that has changed to a cart state, segment checks endstops",
			{}, { MakeDm(DMState::cartLinear, true, false, Local) }, true });

	// A move that checks endstops is committed but its segments are not due yet: NewSegment has set the state 'starting' and copied
	// the segment flags. The step ISR checks the endstops when the segment is due, so the input change must not stop anything yet.
	Run({ "homing move not started yet (DM starting), print move finishing",
			{ MakeDm(DMState::cartDecelNoReverse, false, false, Local), MakeDm(DMState::starting, true, false, Remote) }, {}, false });
	Run({ "phase stepped homing move not started yet (DM starting)",
			{}, { MakeDm(DMState::starting, true, false, Local) }, false });

	// DeactivateDM makes a DM idle when it takes it out of its list and leaves its segment flags as they were. The state alone must
	// decide, so such a DM must not count even when it is linked
	Run({ "idle DM with stale flags that check endstops",
			{ MakeDm(DMState::idle, true, false, Local) }, {}, false });

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
