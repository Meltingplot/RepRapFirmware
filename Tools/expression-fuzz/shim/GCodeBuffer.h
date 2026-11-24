// Expression fuzz stand-in for GCodes/GCodeBuffer/GCodeBuffer.h, found by the "GCodeBuffer.h" include of the copy of
// ExpressionParser.cpp in gen/ and by the <GCodes/GCodeBuffer/GCodeBuffer.h> include of Variable.cpp. The members that
// ExpressionParser.cpp calls, with the names and types of the real class (GCodeBuffer.h:87, 94, 96, 347; GCodeMachineState.h:327)
#ifndef EXPRESSION_FUZZ_GCODEBUFFER_H
#define EXPRESSION_FUZZ_GCODEBUFFER_H
#include <ExprShim.h>
#include <ObjectModel/ObjectModel.h>
#include <ObjectModel/Variable.h>

class GCodeMachineState
{
public:
	int32_t GetIterations() const noexcept { return iterations; }		// GCodeMachineState.h:327: -1 when not inside a loop
	int32_t iterations = -1;
	uint32_t lineNumber = 0;
};

class GCodeBuffer
{
public:
	int32_t GetLineNumber() const noexcept { return machineState.lineNumber; }
	GCodeResult GetLastResult() const noexcept { return lastResult; }
	ExpressionValue GetM291Result() const noexcept { return m291Result; }
	VariableSet& GetVariables() const noexcept { return variables; }
	GCodeMachineState& CurrentFileMachineState() const noexcept { return machineState; }
	bool IsDoingFileMacro() const noexcept { return false; }
	bool IsDoingFile() const noexcept { return false; }
	// For GCodeException::GetMessage (GCodeException.cpp:68-94): the buffer runs "M118" (echo), whose parameter is the expression
	char GetCommandLetter() const noexcept { return 'M'; }
	bool HasCommandNumber() const noexcept { return true; }
	int GetCommandNumber() const noexcept { return 118; }
	int8_t GetCommandFraction() const noexcept { return -1; }

	mutable GCodeMachineState machineState;
	mutable VariableSet variables;
	GCodeResult lastResult = GCodeResult::ok;
	ExpressionValue m291Result;
};
#endif
