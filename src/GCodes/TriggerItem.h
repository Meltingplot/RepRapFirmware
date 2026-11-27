/*
 * Trigger.h
 *
 *  Created on: 6 Jun 2019
 *      Author: David
 */

#ifndef SRC_GCODES_TRIGGERITEM_H_
#define SRC_GCODES_TRIGGERITEM_H_

#include <RepRapFirmware.h>
#include <Hardware/IoPorts.h>
#include <Platform/StringHandle.h>

// What a trigger does when it fires, set by the M581 K parameter. All but runMacro are hard-wired, so that they do not depend on the file system
enum class TriggerAction : uint8_t { emergencyStop = 0, pause, runMacro, feedHold, feedHoldAndAbort };
constexpr unsigned int NumTriggerActions = 5;

class TriggerItem
{
public:
	TriggerItem() noexcept;

	void Init() noexcept;

	// Return true if this trigger is unused, i.e. it doesn't watch any pins
	bool IsUnused() const noexcept;

	// Return what this trigger does when it fires. Without a K parameter trigger 0 is an emergency stop, trigger 1 pauses and all others run their macro
	TriggerAction GetAction(unsigned int number) const noexcept;

	// Return true if the level of an input, endstop or expression that fires this trigger is active now, as of the last call to Check
	bool IsLevelActive() const noexcept;

	// Check whether this trigger is active and update the input states
	bool Check(unsigned int number) noexcept;

	// Handle M581 for this trigger
	GCodeResult Configure(unsigned int number, GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	// Handle M582 for this trigger
	bool CheckLevel(unsigned int number) noexcept;

	// Return true if the action of this trigger is M112, so that it fires as an emergency stop like trigger 0
	bool IsEmergencyStop() const noexcept { return actionIsEmergencyStop; }

	// Return true if this trigger runs an action line instead of triggerN.g
	bool HasActionLine() const noexcept { return !actionLine.IsNull(); }

	// Copy the action line of this trigger
	void GetActionLine(const StringRef& str) const noexcept;

	// Return true if an action may contain this command
	static bool IsAllowedInAction(char letter, int number) noexcept;

private:
	bool EvaluateExpression() THROWS(GCodeException);
	static bool IsAllowedAction(const char *_ecv_array s) noexcept;
	static void AppendInputNames(AxesBitmap endstops, InputPortsBitmap inputs, const StringRef& reply) noexcept;

	AxesBitmap highLevelEndstops, lowLevelEndstops, endstopStates;
	InputPortsBitmap highLevelInputs, lowLevelInputs, inputStates;
	AutoStringHandle expr;					// the expression to trigger on, or null if we are triggering on inputs/endstops
	AutoStringHandle actionLine;			// the line of G-code to run instead of triggerN.g (U parameter), or null
	uint32_t whenBecameTrue;				// when the expression became true, while confirming
	uint16_t confirmMillis;					// how long the expression must stay true before the trigger fires (D parameter)
	int8_t condition;						// the condition specified by the R parameter
	int8_t action;							// the action specified by the K parameter, or -1 to use the default for the trigger number
	bool exprResult;						// the value of the expression when we last evaluated it
	bool confirming;						// the expression has become true and has not stayed true for confirmMillis yet
	bool actionIsEmergencyStop;				// the action is M112
	bool locked;							// trigger 0 or a trigger with an action has been configured, so it cannot be changed until the next restart
};

#endif /* SRC_GCODES_TRIGGERITEM_H_ */
