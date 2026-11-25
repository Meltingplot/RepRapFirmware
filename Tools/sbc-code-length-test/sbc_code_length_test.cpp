// Host test of the code length limits of a G-code buffer in SBC mode, compiled from the real text of StringParser::Put,
// StringParser::AddToChecksum and StringParser::StoreAndAddToChecksum (src/GCodes/GCodeBuffer/StringParser.cpp), of BinaryParser::Put
// and BinaryParser::ParametersFit (src/GCodes/GCodeBuffer/BinaryParser.cpp), of GCodeBuffer::AllocateBinaryBuffer
// (src/GCodes/GCodeBuffer/GCodeBuffer.cpp) and of MaxGCodeStringLength (src/Config/Configuration.h) and MaxGCodeBinaryLength
// (src/SBC/SbcMessageFormats.h), which the Makefile extracts.
//
// A G-code buffer allocates its buffer once, for its first code. Text codes may hold MaxGCodeStringLength characters including the
// terminating null, and DSF sends binary codes of up to MaxGCodeBinaryLength bytes (DSF Settings.MaxCodeBufferSize; RRF refuses longer
// ones in the SBC task). In SBC mode a buffer can get both kinds of code: USB, Aux and the codes that RRF puts itself are text, and the
// macros that DSF runs on the same channel are binary. So whichever code comes first, the buffer must take a text line of
// MaxGCodeStringLength - 1 characters and a binary code of MaxGCodeBinaryLength bytes.
//
// The test plays the G-code buffer, RepRap and Tasks::AllocPermanent with stand-ins, puts text with StringParser::Put character by
// character and binary codes with BinaryParser::Put, after GCodeBuffer::AllocateBinaryBuffer if the buffer has none yet, as
// SbcInterface::FillBuffer does, and checks which codes are stored and which are marked as too long
// (GCodeBuffer::overflowed, which becomes "GCode command too long" in StringParser::CheckMetaCommand and GCodes::ActOnCode). It also
// checks that a binary code whose parameters do not fit in it is marked as malformed instead of stored (GCodeBuffer::malformed, which
// becomes "GCode command malformed" in GCodes::ActOnCode).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#define HAS_SBC_INTERFACE 1
#define SUPPORT_ASYNC_MOVES 1							// as on the MB6HC

using std::min;

#include "Lengths.inc"

typedef uint32_t FilePosition;

// As in src/GCodes/GCodeBuffer/GCodeBuffer.h
enum class GCodeBufferState : uint8_t
{
	parseNotStarted, parsingLineNumber, parsingWhitespace, parsingComment, parsingGCode, parsingBracketedComment, parsingQuotedString,
	parsingChecksum, discarding, ready, executing, executingOnSbc
};

// As in src/SBC/SbcMessageFormats.h
enum CodeFlags : uint8_t
{
	HasMajorCommandNumber = 1,
	HasMinorCommandNumber = 2,
	HasFilePosition = 4,
	EnforceAbsolutePosition = 8,
	HasExplicitLineNumber = 16
};

enum class DataType : uint8_t
{
    Int = 0,				// int32_t
    UInt = 1,				// uint32_t
    Float = 2,				// float
    IntArray = 3,			// int32_t[]
    UIntArray = 4,			// uint32_t[]
    FloatArray = 5,			// float[]
    String = 6,				// char[]
    Expression = 7,			// char[] but containing '{'...'}'
	DriverId_dt = 8,		// two sequential uint16_t representing board and port of a driver
	DriverIdArray = 9,		// array of driver ids
	Boolean = 10,			// bool (int32_t)
	BoolArray = 11,			// bool[] (uint8_t[])
	ULong = 12,				// uint64_t
	DateTime = 13,			// datetime string in ISO-conform formatting
	Null = 14,				// null value
	Char = 15,				// char value (int32_t)
	Bitmap16 = 16,			// 16-bit bitmap
	Bitmap32 = 17,			// 32-bit bitmap
	Bitmap64 = 18,			// 64-bit bitmap
	FloatWithDigits = 19	// float with a specified number of digits (from RRF to SBC only when returning expression values)
};

struct CodeHeader
{
	uint8_t channel;
	CodeFlags flags;
	uint8_t numParameters;
	char letter;
	int32_t majorCode;
	int32_t minorCode;
	uint32_t filePosition;
	int32_t lineNumber;
};

struct CodeParameter
{
	char letter;
	DataType type;
	uint16_t padding;
	union
	{
		int32_t intValue;
		uint32_t uintValue;
		float floatValue;
	};
};

enum class MachineType : uint8_t { fff, laser, cnc };

static bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

static void memcpyu32(uint32_t *dst, const uint32_t *src, size_t numWords) noexcept
{
	memcpy(dst, src, numWords * sizeof(uint32_t));
}

// What RepRap and GCodes give the parsers
class GCodes
{
public:
	MachineType GetMachineType() const noexcept { return MachineType::fff; }
};

class RepRap
{
public:
	bool UsingSbcInterface() const noexcept { return usingSbcInterface; }
	GCodes& GetGCodes() noexcept { return gCodes; }

	bool usingSbcInterface = true;

private:
	GCodes gCodes;
};

static RepRap reprap;

// Tasks::AllocPermanent records the size of each allocation
static std::vector<size_t> allocations;
static std::vector<std::unique_ptr<uint32_t[]>> allocatedMemory;

namespace Tasks
{
	void *AllocPermanent(size_t sz, std::align_val_t align) noexcept
	{
		(void)align;
		allocations.push_back(sz);
		allocatedMemory.emplace_back(new uint32_t[(sz + 3) / 4]());
		return allocatedMemory.back().get();
	}
}

class CRC16
{
public:
	void Reset(uint16_t initialValue) noexcept { crc = initialValue; }
	void Update(char c) noexcept { crc = (uint16_t)(crc + (uint8_t)c); }

private:
	uint16_t crc = 0;
};

struct MachineState
{
	bool g53Active = false;
	int32_t lineNumber = 0;
	FilePosition fpos = 0;
};

// Just the members of GCodeBuffer (src/GCodes/GCodeBuffer/GCodeBuffer.h) that the extracted functions use
class GCodeBuffer
{
public:
	bool IsDoingFile() const noexcept { return false; }
	MachineState& LatestMachineState() noexcept { return machineState; }
	MachineState& CurrentFileMachineState() noexcept { return machineState; }
	bool HasBuffer() const noexcept { return buffer != nullptr; }			// as in GCodeBuffer.h
	void AllocateBinaryBuffer() noexcept;

	char *buffer = nullptr;
	size_t bufferLength = 0;
	GCodeBufferState bufferState = GCodeBufferState::parseNotStarted;
	bool overflowed = false;
	bool malformed = false;

private:
	MachineState machineState;
};

// The members of StringParser (src/GCodes/GCodeBuffer/StringParser.h) that the extracted functions use
class StringParser
{
public:
	explicit StringParser(GCodeBuffer& gcodeBuffer) noexcept : gb(gcodeBuffer) { }

	bool Put(char c) noexcept;

	// What StringParser::Init does for the members that Put uses, for the next line
	void StartLine() noexcept
	{
		gb.bufferState = GCodeBufferState::parseNotStarted;
		gb.overflowed = false;
		commandLength = gcodeLineEnd = commandStart = braceCount = 0;
		commandIndent = 0;
		hadLineNumber = hadChecksum = seenLeadingSpace = seenLeadingTab = seenExpression = false;
	}

	unsigned int StoredLength() const noexcept { return gcodeLineEnd; }

private:
	void AddToChecksum(char c) noexcept;
	void StoreAndAddToChecksum(char c) noexcept;

	// The end of a line: the stand-in only terminates it, the checks of the real LineFinished do not touch the limits
	bool LineFinished() noexcept
	{
		gb.buffer[gcodeLineEnd] = 0;
		gb.bufferState = GCodeBufferState::ready;
		return true;
	}

	GCodeBuffer& gb;
	unsigned int commandStart = 0;
	unsigned int commandLength = 0;
	unsigned int braceCount = 0;
	unsigned int gcodeLineEnd = 0;
	unsigned int receivedLineNumber = 0;
	unsigned int declaredChecksum = 0;
	CRC16 crc16;
	uint8_t computedChecksum = 0;
	uint8_t checksumCharsReceived = 0;
	uint8_t commandIndent = 0;
	static constexpr uint8_t MaxIndent = 0xFF;
	bool hadLineNumber = false;
	bool hadChecksum = false;
	bool seenLeadingSpace = false;
	bool seenLeadingTab = false;
	bool seenExpression = false;
};

// The members of BinaryParser (src/GCodes/GCodeBuffer/BinaryParser.h) that the extracted functions use
class BinaryParser
{
public:
	explicit BinaryParser(GCodeBuffer& gcodeBuffer) noexcept : gb(gcodeBuffer) { }

	void Put(const uint32_t *data, size_t len) noexcept;
	bool HasCode() const noexcept { return bufferLength != 0; }

private:
	FilePosition GetFilePosition() const noexcept { return 0; }
	bool ParametersFit(size_t codeLength) const noexcept;
	size_t AddPadding(size_t bytesRead) const noexcept { return (bytesRead + 3u) & (~3u); }	// as in BinaryParser.h

	GCodeBuffer& gb;
	size_t bufferLength = 0;
	CodeHeader *header = nullptr;
};

#include "StringParser.inc"
#include "BinaryParser.inc"
#include "GCodeBuffer.inc"

// A G-code buffer with both parsers, as GCodeBuffer has them
struct Channel
{
	GCodeBuffer gb;
	StringParser stringParser{gb};
	BinaryParser binaryParser{gb};

	// Put a text line character by character and its end. Return true if it was stored completely
	bool PutText(const std::string& line) noexcept
	{
		stringParser.StartLine();
		for (char c : line)
		{
			(void)stringParser.Put(c);
		}
		(void)stringParser.Put('\n');
		return !gb.overflowed && stringParser.StoredLength() == line.size() && strcmp(gb.buffer, line.c_str()) == 0;
	}

	// As SbcInterface::FillBuffer: allocate the buffer before the first binary code, then put the code
	void PutAsFillBuffer(const std::vector<uint32_t>& code) noexcept
	{
		if (!gb.HasBuffer())
		{
			gb.AllocateBinaryBuffer();
		}
		binaryParser.Put(code.data(), code.size());
	}

	// Put a binary code of the given length in bytes. Return true if it was stored
	bool PutBinary(size_t length) noexcept
	{
		std::vector<uint32_t> code(length / sizeof(uint32_t), 0x5A5A5A5Au);
		CodeHeader header;
		memset(&header, 0, sizeof(header));
		header.letter = 'M';
		header.majorCode = 98;
		memcpy(code.data(), &header, sizeof(header));
		PutAsFillBuffer(code);
		return !gb.overflowed && !gb.malformed && binaryParser.HasCode() && memcmp(gb.buffer, code.data(), length) == 0;
	}

	// Put a binary code of the given length in bytes with one parameter of the given type and intValue, whose value starts right after
	// the parameter table. Return true if it was stored
	bool PutBinaryWithParameter(size_t length, DataType type, uint32_t value) noexcept
	{
		std::vector<uint32_t> code(length / sizeof(uint32_t), 0x5A5A5A5Au);
		CodeHeader header;
		memset(&header, 0, sizeof(header));
		header.letter = 'M';
		header.majorCode = 98;
		header.numParameters = 1;
		memcpy(code.data(), &header, sizeof(header));
		if (length >= sizeof(CodeHeader) + sizeof(CodeParameter))
		{
			CodeParameter param;
			memset(&param, 0, sizeof(param));
			param.letter = 'P';
			param.type = type;
			param.uintValue = value;
			memcpy(reinterpret_cast<char *>(code.data()) + sizeof(CodeHeader), &param, sizeof(param));
		}
		PutAsFillBuffer(code);
		return !gb.overflowed && !gb.malformed && binaryParser.HasCode() && memcmp(gb.buffer, code.data(), length) == 0;
	}
};

// A text line of the given number of characters
static std::string Line(size_t length) noexcept
{
	std::string line = "M117 \"";
	line.append(length - line.size() - 1, 'x');
	line += '"';
	return line;
}

static unsigned int passed = 0, failed = 0;

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
		printf("FAIL %s:%s\n", name, why.c_str());
	}
	allocations.clear();
}

int main()
{
	constexpr size_t MaxTextLine = MaxGCodeStringLength - 1;				// characters, without the terminating null
	constexpr size_t MaxBinary = MaxGCodeBinaryLength;						// bytes
	printf("MaxGCodeStringLength = %zu, MaxGCodeBinaryLength = %zu\n", MaxGCodeStringLength, MaxGCodeBinaryLength);

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		if (!ch.PutText("M115")) { why += " the text code was not stored"; }
		if (allocations.size() != 1 || allocations[0] != MaxGCodeBinaryLength) { why += " the buffer was not allocated with MaxGCodeBinaryLength bytes"; }
		if (!ch.PutBinary(MaxGCodeStringLength + 44)) { why += " a binary code of " + std::to_string(MaxGCodeStringLength + 44) + " bytes was refused"; }
		if (!ch.PutBinary(MaxBinary)) { why += " a binary code of " + std::to_string(MaxBinary) + " bytes was refused"; }
		Report("SBC mode, first code text: binary codes up to MaxGCodeBinaryLength bytes are stored", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		if (!ch.PutBinary(MaxBinary)) { why += " a binary code of " + std::to_string(MaxBinary) + " bytes was refused"; }
		if (allocations.size() != 1 || allocations[0] != MaxGCodeBinaryLength) { why += " the buffer was not allocated with MaxGCodeBinaryLength bytes"; }
		Report("SBC mode, first code binary: binary codes up to MaxGCodeBinaryLength bytes are stored", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		if (!ch.PutText(Line(MaxTextLine))) { why += " a text line of " + std::to_string(MaxTextLine) + " characters was refused"; }
		if (ch.PutText(Line(MaxTextLine + 1))) { why += " a text line of " + std::to_string(MaxTextLine + 1) + " characters was stored"; }
		Report("SBC mode, first code text: text lines keep the limit of MaxGCodeStringLength - 1 characters", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		if (!ch.PutBinary(sizeof(CodeHeader))) { why += " the binary code was not stored"; }
		if (!ch.PutText(Line(MaxTextLine))) { why += " a text line of " + std::to_string(MaxTextLine) + " characters was refused"; }
		if (ch.PutText(Line(MaxTextLine + 1))) { why += " a text line of " + std::to_string(MaxTextLine + 1) + " characters was stored"; }
		Report("SBC mode, first code binary: text lines keep the limit of MaxGCodeStringLength - 1 characters", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		if (!ch.PutText("M115")) { why += " the text code was not stored"; }
		if (ch.PutBinary(MaxBinary + 4)) { why += " a binary code of " + std::to_string(MaxBinary + 4) + " bytes was stored"; }
		Report("SBC mode: binary codes longer than MaxGCodeBinaryLength are refused", why);
	}

	{
		reprap.usingSbcInterface = false;
		Channel ch;
		std::string why;
		if (!ch.PutText(Line(MaxTextLine))) { why += " a text line of " + std::to_string(MaxTextLine) + " characters was refused"; }
		if (allocations.size() != 1 || allocations[0] != MaxGCodeStringLength) { why += " the buffer was not allocated with MaxGCodeStringLength bytes"; }
		if (ch.PutText(Line(MaxTextLine + 1))) { why += " a text line of " + std::to_string(MaxTextLine + 1) + " characters was stored"; }
		Report("standalone mode: the buffer has MaxGCodeStringLength bytes and text lines their limit", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		constexpr size_t LongestString = MaxBinary - sizeof(CodeHeader) - sizeof(CodeParameter);
		if (!ch.PutBinaryWithParameter(MaxBinary, DataType::String, LongestString))
		{
			why += " a code of " + std::to_string(MaxBinary) + " bytes whose string ends at its last byte was refused";
		}
		if (!ch.PutBinaryWithParameter(MaxBinary, DataType::IntArray, LongestString / sizeof(uint32_t)))
		{
			why += " a code of " + std::to_string(MaxBinary) + " bytes whose array ends at its last byte was refused";
		}
		if (!ch.PutBinaryWithParameter(sizeof(CodeHeader) + sizeof(CodeParameter), DataType::Int, 0xFFFFFFFFu))
		{
			why += " a code whose only parameter holds its value itself was refused";
		}
		Report("SBC mode: binary codes whose parameters end at the end of the code are stored", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		constexpr size_t LongestString = MaxBinary - sizeof(CodeHeader) - sizeof(CodeParameter);
		struct { const char *what; size_t length; DataType type; uint32_t value; } cases[] =
		{
			{ "a parameter table beyond the code", sizeof(CodeHeader), DataType::Int, 0 },
			{ "a string one byte beyond the code", MaxBinary, DataType::String, LongestString + 1 },
			{ "an array one element beyond the code", MaxBinary, DataType::IntArray, LongestString / sizeof(uint32_t) + 1 },
			{ "a negative string length", MaxBinary, DataType::String, 0xFFFFFFFFu },
			{ "a negative array length", MaxBinary, DataType::FloatArray, 0x80000000u },
		};
		for (const auto& c : cases)
		{
			if (ch.PutBinaryWithParameter(c.length, c.type, c.value))
			{
				why += std::string(" a code with ") + c.what + " was stored";
			}
			else if (!ch.gb.malformed || ch.gb.overflowed || ch.binaryParser.HasCode() || ch.gb.bufferState != GCodeBufferState::executing)
			{
				why += std::string(" a code with ") + c.what + " was not marked as malformed only";
			}
		}
		Report("SBC mode: binary codes whose parameters do not fit in them are marked as malformed", why);
	}

	{
		reprap.usingSbcInterface = true;
		Channel ch;
		std::string why;
		(void)ch.PutBinaryWithParameter(sizeof(CodeHeader), DataType::Int, 0);
		if (!ch.PutBinary(MaxBinary)) { why += " a good code after a malformed one was refused or still marked as malformed"; }
		(void)ch.PutBinaryWithParameter(sizeof(CodeHeader), DataType::Int, 0);
		(void)ch.PutBinary(MaxBinary + 4);
		if (!ch.gb.overflowed || ch.gb.malformed) { why += " a code that is too long after a malformed one was not marked as too long only"; }
		Report("SBC mode: the next binary code clears the malformed mark", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
