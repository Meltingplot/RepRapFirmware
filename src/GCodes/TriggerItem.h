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

class TriggerItem
{
public:
	TriggerItem() noexcept;

	void Init() noexcept;

	// Return true if this trigger is unused, i.e. it doesn't watch any pins
	bool IsUnused() const noexcept;

	// Check whether this trigger is active and update the input states
	bool Check(unsigned int number) noexcept;

	// Handle M581 for this trigger
	GCodeResult Configure(unsigned int number, GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	// Handle M582 for this trigger
	bool CheckLevel(unsigned int number) noexcept;

	// Return true if the action of this trigger is M112, so that it fires as an emergency stop like trigger 0
	bool IsEmergencyStop() const noexcept { return actionIsEmergencyStop; }

	// Return true if this trigger runs an action instead of triggerN.g
	bool HasAction() const noexcept { return !action.IsNull(); }

	// Copy the action of this trigger
	void GetAction(const StringRef& str) const noexcept;

	// Return true if an action may contain this command
	static bool IsAllowedInAction(char letter, int number) noexcept;

private:
	bool EvaluateExpression() THROWS(GCodeException);
	static bool IsAllowedAction(const char *_ecv_array s) noexcept;
	static void AppendInputNames(AxesBitmap endstops, InputPortsBitmap inputs, const StringRef& reply) noexcept;

	AxesBitmap highLevelEndstops, lowLevelEndstops, endstopStates;
	InputPortsBitmap highLevelInputs, lowLevelInputs, inputStates;
	AutoStringHandle expr;					// the expression to trigger on, or null if we are triggering on inputs/endstops
	AutoStringHandle action;				// the line of G-code to run instead of triggerN.g (U parameter), or null
	uint32_t whenBecameTrue;				// when the expression became true, while confirming
	uint16_t confirmMillis;					// how long the expression must stay true before the trigger fires (D parameter)
	int8_t condition;						// the condition specified by the R parameter
	bool exprResult;						// the value of the expression when we last evaluated it
	bool confirming;						// the expression has become true and has not stayed true for confirmMillis yet
	bool actionIsEmergencyStop;				// the action is M112
	bool locked;							// trigger 0 or a trigger with an action has been configured, so it cannot be changed until the next restart
};

#endif /* SRC_GCODES_TRIGGERITEM_H_ */
