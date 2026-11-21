// Host stand-in for GCodes/GCodeBuffer/GCodeBuffer.h: the members that GCodeException.cpp calls. GetMessage puts the command,
// e.g. "G1: ", in front of the message, as GCodeMachineState::RetrieveStateMachineResult does with the buffer of the command.

#ifndef GCODE_EXCEPTION_TEST_GCODEBUFFER_H
#define GCODE_EXCEPTION_TEST_GCODEBUFFER_H

#include <RepRapFirmware.h>
#include <Platform/StringHandle.h>

class GCodeBuffer
{
public:
	GCodeBuffer(char letter, int number) noexcept : commandLetter(letter), commandNumber(number) { }

	bool IsDoingFileMacro() const noexcept { return false; }
	bool IsDoingFile() const noexcept { return false; }
	int32_t GetLineNumber() const noexcept { return -1; }
	const AutoStringHandle& GetFileName() const noexcept { return fileName; }
	char GetCommandLetter() const noexcept { return commandLetter; }
	bool HasCommandNumber() const noexcept { return true; }
	int GetCommandNumber() const noexcept { return commandNumber; }
	int8_t GetCommandFraction() const noexcept { return -1; }

private:
	AutoStringHandle fileName;
	char commandLetter;
	int commandNumber;
};

#endif
