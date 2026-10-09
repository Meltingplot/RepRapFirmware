/*
 * Trigger.cpp
 *
 *  Created on: 6 Jun 2019
 *      Author: David
 */

#include "TriggerItem.h"
#include <Platform/RepRap.h>
#include "GCodes.h"
#include <PrintMonitor/PrintMonitor.h>
#include "GCodeBuffer/GCodeBuffer.h"
#include "GCodeBuffer/ExpressionParser.h"

TriggerItem::TriggerItem() noexcept : condition(0)
{
}

// Initialise the trigger
void TriggerItem::Init() noexcept
{
	highLevelEndstops.Clear();
	lowLevelEndstops.Clear();
	highLevelInputs.Clear();
	lowLevelInputs.Clear();
	expr.Delete();
	action.Delete();
	confirmMillis = 0;
	confirming = false;
	actionIsEmergencyStop = false;
	condition = -1;
}

// Return true if this trigger is unused, i.e. it doesn't watch any pins
bool TriggerItem::IsUnused() const noexcept
{
	return (highLevelEndstops | lowLevelEndstops).IsEmpty() && (highLevelInputs | lowLevelInputs).IsEmpty() && expr.IsNull();
}

// Check whether this trigger is active and update the input states. This is called in a polling loop, so it needs to be fast.
bool TriggerItem::Check(unsigned int number) noexcept
{
	if (condition < 0) { return false; }			// don't check if the trigger is disabled

	bool triggered = false;

	if (expr.IsNull())
	{
		Platform& platform = reprap.GetPlatform();

		// Check the endstops
		const AxesBitmap endstopsMonitored = highLevelEndstops | lowLevelEndstops;
		if (endstopsMonitored.IsNonEmpty())
		{
			EndstopsManager& endstops = platform.GetEndstops();
			endstopsMonitored.Iterate([this, &endstops, &triggered](unsigned int axis, unsigned int) noexcept
										{
											const bool stopped = endstops.Stopped(axis);
											if (stopped != endstopStates.IsBitSet(axis))
											{
												if (stopped)
												{
													endstopStates.SetBit(axis);
													if (highLevelEndstops.IsBitSet(axis))
													{
														triggered = true;
													}
												}
												else
												{
													endstopStates.ClearBit(axis);
													if (lowLevelEndstops.IsBitSet(axis))
													{
														triggered = true;
													}
												}
											}
										}
									);
		}

		const InputPortsBitmap portsMonitored = highLevelInputs | lowLevelInputs;
		if (portsMonitored.IsNonEmpty())
		{
			portsMonitored.Iterate([this, &platform, &triggered](unsigned int inPort, unsigned int) noexcept
									{
										const bool isActive = reprap.GetPlatform().GetGpInPort(inPort).GetState();
										if (isActive != inputStates.IsBitSet(inPort))
										{
											if (isActive)
											{
												inputStates.SetBit(inPort);
												if (highLevelInputs.IsBitSet(inPort))
												{
													triggered = true;
												}
											}
											else
											{
												inputStates.ClearBit(inPort);
												if (lowLevelInputs.IsBitSet(inPort))
												{
													triggered = true;
												}
											}

										}
									}
								);
		}
	}
	else
	{
		// We have an expression to evaluate
		try
		{
			const bool oldVal = exprResult;
			exprResult = EvaluateExpression();
			if (!exprResult)
			{
				confirming = false;
			}
			else if (!oldVal && !confirming)
			{
				confirming = true;							// the expression has become true, so it must now stay true for confirmMillis
				whenBecameTrue = millis();
			}
			triggered = confirming && millis() - whenBecameTrue >= confirmMillis;
			if (triggered)
			{
				confirming = false;
			}
		}
		catch (const GCodeException& e)
		{
			String<StringLength256> errorMessage;
			e.GetMessage(errorMessage.GetRef(), nullptr);
			if (number == 0 || actionIsEmergencyStop)
			{
				// Trigger 0 and triggers whose action is M112 are emergency stops: if the expression cannot be evaluated, fire rather than disable
				errorMessage.catf("\nTrigger %u fired\n", number);
				reprap.GetPlatform().Message(ErrorMessage, errorMessage.c_str());
				return true;
			}
			condition = -1;
			errorMessage.catf("\nTrigger %u disabled\n", number);
			reprap.GetPlatform().Message(ErrorMessage, errorMessage.c_str());
		}
	}

	return triggered &&
			(   condition == 0
			 || (2 - condition == (int)reprap.GetPrintMonitor().IsPrinting())		// condition == 1 && IsPrinting || condition == 2 && !IsPrinting
			);
}

// Handle M581 and M581.1 for this trigger. We have already checked that gb.GetCommandFraction() returns <= 1.
GCodeResult TriggerItem::Configure(unsigned int number, GCodeBuffer &gb, const StringRef &reply) THROWS(GCodeException)
{
	// We allow the P-1 parameter to be used with both M581 and M581.1
	bool seen = gb.Seen('P');
	if (seen)
	{
		// We need a try..catch block here so that if we pass an array of unsigned or a string we do not abort when trying to read a single integer
		try
		{
			if (gb.GetIValue() == -1)
			{
				Init();						// P-1 deletes the trigger
				condition = -1;
				return GCodeResult::ok;
			}
		} catch (const GCodeException&) { }
	}

	const bool wasUnused = IsUnused();		// save for later

	switch (gb.GetCommandFraction())
	{
	case 1:									// trigger on an expression
		{
			// Read U and D before anything is changed, so that an error in them leaves the trigger as it was.
			// The U parameter is G-code to run instead of triggerN.g; "\n" separates its lines. An action that is just M112 is an emergency stop.
			const bool seenAction = gb.Seen('U');
			if (seenAction && number < 2)
			{
				reply.copy("Triggers 0 and 1 cannot have an action");
				return GCodeResult::error;
			}
			String<StringLength256> actionString;
			if (seenAction)
			{
				gb.GetQuotedString(actionString.GetRef(), true);		// may throw
				const char *_ecv_array _ecv_null const badLine = FindDisallowedLine(actionString.c_str());
				if (badLine != nullptr)
				{
					const char *_ecv_array const badLineEnd = strstr(badLine, "\\n");
					reply.printf("not allowed in an action, put it in trigger%u.g: %.*s", number, (badLineEnd == nullptr) ? 50 : min<int>(badLineEnd - badLine, 50), badLine);
					return GCodeResult::error;
				}
			}
			uint32_t confirmTime = confirmMillis;
			(void)gb.TryGetLimitedUIValue('D', confirmTime, seen, 65536);	// may throw

			if (gb.Seen('P'))
			{
				highLevelInputs.Clear();
				lowLevelInputs.Clear();
				highLevelEndstops.Clear();
				lowLevelEndstops.Clear();
				String<StringLength256> conditionString;
				gb.GetQuotedString(conditionString.GetRef(), false);		// may throw
				expr.Assign(conditionString.c_str());
			}
			if (seenAction)
			{
				action.Assign(actionString.c_str());						// U"" deletes the action, so that triggerN.g runs again
				const char *_ecv_array p = actionString.c_str() + strspn(actionString.c_str(), " \t");
				actionIsEmergencyStop = StringStartsWithIgnoreCase(p, "M112");
				if (actionIsEmergencyStop)
				{
					p += 4 + strspn(p + 4, " \t");
					actionIsEmergencyStop = (*p == 0 || *p == ';');			// M112 with nothing but spaces or a comment around it
				}
				seen = true;
			}
			confirmMillis = (uint16_t)confirmTime;
		}
		break;

	default:								// trigger on inputs and/or endstops
		{
			const int sParam = (gb.Seen('S')) ? gb.GetIValue() : 1;			// S is ignored if there is no P or axis letter parameter, so don't set 'seen'

			// See if there are inputs to trigger on
			if (gb.Seen('P'))
			{
				expr.Delete();
				uint32_t inputNumbers[MaxGpInPorts];
				size_t numValues = MaxGpInPorts;
				gb.GetUnsignedArray(inputNumbers, numValues, false);
				const InputPortsBitmap portsToWaitFor = InputPortsBitmap::MakeFromArray(inputNumbers, numValues);
				if (sParam < 0)
				{
					highLevelInputs &= ~portsToWaitFor;
					lowLevelInputs &= ~portsToWaitFor;
				}
				else
				{
					((sParam >= 1) ? highLevelInputs : lowLevelInputs) |= portsToWaitFor;
				}
			}

			// See if there are endstops to trigger on
			{
				AxesBitmap endstopsToWaitFor;
				for (size_t axis = 0; axis < reprap.GetGCodes().GetTotalAxes(); ++axis)
				{
					if (gb.Seen(reprap.GetGCodes().GetAxisLetters()[axis]))
					{
						seen = true;
						expr.Delete();
						endstopsToWaitFor.SetBit(axis);
					}
				}
				if (endstopsToWaitFor.IsNonEmpty())
				{
					if (sParam < 0)
					{
						highLevelEndstops &= ~endstopsToWaitFor;
						lowLevelEndstops &= ~endstopsToWaitFor;
					}
					else
					{
						((sParam >= 1) ? highLevelEndstops : lowLevelEndstops) |= endstopsToWaitFor;
					}
				}
			}

			if (seen)
			{
				action.Delete();									// a trigger on inputs or endstops runs triggerN.g
				actionIsEmergencyStop = false;
			}
		}
	}

	if (!IsUnused())
	{
		if (gb.Seen('R'))
		{
			condition = gb.GetIValue();
			seen = true;
		}
		else if (seen && wasUnused)
		{
			condition = 0;										// this is a new trigger, so set no enable condition
		}
	}

	if (seen)
	{
		// If trigger inputs or the enable condition have been changed, determine the initial state
		if (expr.IsNull())
		{

			inputStates.Clear();
			(void)Check(number);								// set up initial input states
		}
		else
		{
			// Get the initial value of the expression
			confirming = false;
			try
			{
				exprResult = EvaluateExpression();				// may throw
			}
			catch (const GCodeException&)
			{
				if (number != 0 && !actionIsEmergencyStop)		// keep an emergency stop trigger, so that its next check fires it
				{
					Init();										// clear the trigger
				}
				throw;											// report the error
			}
		}
	}
	else
	{
		reply.printf("Trigger %u ", number);
		if (IsUnused())
		{
			reply.cat("is not configured");
		}
		else
		{
			if (condition < 0)
			{
				reply.cat("if enabled would fire");
			}
			else if (condition == 1)
			{
				reply.cat("fires only when printing");
			}
			else if (condition == 2)
			{
				reply.cat("fires only when not printing");
			}
			else
			{
				reply.cat("fires");
			}
			if (expr.IsNull())
			{
				reply.cat(" on a");
				const bool hasHighLevel = !highLevelEndstops.IsEmpty() || !highLevelInputs.IsEmpty();
				if (hasHighLevel)
				{
					reply.cat(" rising edge of endstops/inputs");
					AppendInputNames(highLevelEndstops, highLevelInputs, reply);
				}
				const bool hasLowLevel = !lowLevelEndstops.IsEmpty() || !lowLevelInputs.IsEmpty();
				if (hasLowLevel)
				{
					if (hasHighLevel)
					{
						reply.cat(" or a");
					}
					reply.cat(" falling edge of endstops/inputs");
					AppendInputNames(lowLevelEndstops, lowLevelInputs, reply);
				}
			}
			else
			{
				reply.cat((condition > 0) ? " and" : " when");
				const auto ptr = expr.Get();
				reply.catf(" expression {%s} becomes true", ptr.Ptr());
			}
			if (confirmMillis != 0)
			{
				reply.catf(" and stays true for %ums", (unsigned int)confirmMillis);
			}
			if (!action.IsNull())
			{
				reply.catf(", action \"%s\"", action.Get().Ptr());
			}
		}
	}
	return GCodeResult::ok;
}

// Handle M582 for this trigger
bool TriggerItem::CheckLevel(unsigned int number) noexcept
{
	endstopStates = lowLevelEndstops;
	inputStates = lowLevelInputs;
	exprResult = false;
	confirming = true;										// M582 checks the level without waiting for confirmMillis
	whenBecameTrue = millis() - confirmMillis;
	return Check(number);
}

// Copy the line of the action that starts at 'offset' and advance 'offset' to the next line, returning false if there is no line left.
// The line is copied out while the heap is read-locked, as EvaluateExpression does, so the caller keeps no pointer into the heap.
bool TriggerItem::GetActionLine(uint8_t& offset, const StringRef& line) const noexcept
{
	if (action.IsNull())
	{
		return false;
	}
	const auto ptr = action.Get();
	const size_t length = strlen(ptr.Ptr());
	if (offset >= length)
	{
		return false;
	}
	const char *_ecv_array const lineStart = ptr.Ptr() + offset;
	const char *_ecv_array const lineEnd = strstr(lineStart, "\\n");
	const size_t lineLength = (lineEnd == nullptr) ? length - offset : (size_t)(lineEnd - lineStart);
	line.copy(lineStart, lineLength);
	offset = (uint8_t)((lineEnd == nullptr) ? length : (size_t)(lineEnd - ptr.Ptr()) + 2);	// an action has at most 255 characters
	return true;
}

void TriggerItem::AppendInputNames(AxesBitmap endstops, InputPortsBitmap inputs, const StringRef &reply) noexcept
{
	if (endstops.IsEmpty() && inputs.IsEmpty())
	{
		reply.cat(" (none)");
	}
	else
	{
		const char *_ecv_array const axisLetters = reprap.GetGCodes().GetAxisLetters();
		endstops.Iterate([axisLetters, &reply](unsigned int axis, unsigned int) noexcept { reply.catf(" %c", axisLetters[axis]); } );
		inputs.Iterate([&reply](unsigned int port, unsigned int) noexcept { reply.catf(" %d", port); } );
	}
}

// Return true if an action may contain this command. Actions only contain commands that RRF executes itself and at once:
// nothing that SBC mode passes to DSF or that runs a macro (that belongs in triggerN.g), nothing that waits, and no motion.
bool TriggerItem::IsAllowedInAction(char letter, int number) noexcept
{
	static constexpr int16_t AllowedMCodes[] = { 42, 81, 106, 107, 112, 117, 118, 140, 141, 143, 150, 302, 568, 599 };
	if (letter == 'M')
	{
		for (int16_t code : AllowedMCodes)
		{
			if (code == number)
			{
				return true;
			}
		}
	}
	return false;
}

// Check the lines of an action, returning the first one that it may not contain, or nullptr if they are all allowed.
// Besides the commands of IsAllowedInAction an action may contain set, echo without redirection to a file, comments and blank lines.
const char *_ecv_array _ecv_null TriggerItem::FindDisallowedLine(const char *_ecv_array s) noexcept
{
	for (;;)
	{
		const char *_ecv_array const line = s;
		s += strspn(s, " \t");
		const char *_ecv_array const lineEnd = strstr(s, "\\n");
		if (*s != 0 && *s != ';' && *s != '(' && s != lineEnd)
		{
			bool allowed = false;
			if (StringStartsWith(s, "set") && (s[3] == ' ' || s[3] == '\t'))
			{
				allowed = true;
			}
			else if (StringStartsWith(s, "echo") && (s[4] == ' ' || s[4] == '\t'))
			{
				allowed = s[4 + strspn(s + 4, " \t")] != '>';
			}
			else if (toupper(*s) == 'M' && isDigit(s[1]))
			{
				const char *_ecv_array end;
				allowed = IsAllowedInAction('M', (int)StrToU32(s + 1, &end)) && (*end == 0 || *end == ' ' || *end == '\t' || *end == ';' || end == lineEnd);
			}
			if (!allowed)
			{
				return line;
			}
		}
		if (lineEnd == nullptr)
		{
			return nullptr;
		}
		s = lineEnd + 2;
	}
}

bool TriggerItem::EvaluateExpression() THROWS(GCodeException)
{
	// Copy the expression out of the heap first: the parser takes the heap write lock for string values, which deadlocks while the read lock from Get() is still held
	String<StringLength256> expression;
	expression.copy(expr.Get().Ptr());
	ExpressionParser parser(nullptr, expression.c_str());
	const bool ret = parser.ParseBoolean();
	parser.CheckForExtraCharacters();
	return ret;
}

// End
