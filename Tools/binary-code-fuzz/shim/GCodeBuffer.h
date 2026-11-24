// Binary code fuzz stand-in for GCodes/GCodeBuffer/GCodeBuffer.h, found by the "GCodeBuffer.h" include of BinaryParser.cpp because the
// build compiles a copy of BinaryParser.cpp in gen/ (see the Makefile). The members that BinaryParser uses have the names of the real
// class (BinaryParser is a friend of it); the functions that GCodeException::GetMessage calls go to the binary parser, as
// PARSER_OPERATION does for a binary buffer (GCodeBuffer.cpp)

#ifndef BINARY_CODE_FUZZ_GCODEBUFFER_H
#define BINARY_CODE_FUZZ_GCODEBUFFER_H

#include <BinaryShim.h>
#include <GCodes/GCodeChannel.h>
#include <GCodes/GCodeMachineState.h>
#include <GCodes/GCodeBuffer/BinaryParser.h>

class GCodeBuffer
{
public:
	explicit GCodeBuffer(GCodeChannel ch) noexcept : codeChannel(ch), binaryParser(*this) { }

	GCodeChannel GetChannel() const noexcept { return codeChannel; }
	const char *_ecv_array GetIdentity() const noexcept { return codeChannel.ToString(); }
	GCodeMachineState& LatestMachineState() const noexcept { return machineState; }
	GCodeMachineState& CurrentFileMachineState() const noexcept { return machineState; }
	void SetExplicitLineNumber(uint32_t ln) noexcept { explicitLineNumber = (int32_t)ln; }
	void ClearExplicitLineNumber() noexcept { explicitLineNumber = -1; }

	char GetCommandLetter() const noexcept { return binaryParser.GetCommandLetter(); }
	bool HasCommandNumber() const noexcept { return binaryParser.HasCommandNumber(); }
	int GetCommandNumber() const noexcept { return binaryParser.GetCommandNumber(); }
	int8_t GetCommandFraction() const noexcept { return binaryParser.GetCommandFraction(); }

	GCodeChannel codeChannel;
	mutable GCodeMachineState machineState;
	int32_t explicitLineNumber = -1;

	char *_ecv_array _ecv_null buffer = nullptr;
	size_t bufferLength = 0;
	GCodeBufferState bufferState = GCodeBufferState::parseNotStarted;
	bool overflowed = false;

	BinaryParser binaryParser;
};

#endif
