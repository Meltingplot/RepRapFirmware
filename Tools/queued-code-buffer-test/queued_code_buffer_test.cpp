// Host test of a queued binary code in SBC mode (QueuedCode::AssignTo in src/GCodes/GCodeQueue.cpp), compiled from the real text of the
// function that the Makefile extracts.
//
// A G-code from a file that is to run when the moves before it are done (M3/M4/M5, M42, M104, M106, M107, M140 and others,
// GCodeQueue::ShouldQueueMCode in src/GCodes/GCodeQueue.cpp:35-102) is stored in a QueuedCode (GCodeQueue::QueueCode,
// GCodeQueue.cpp:107-138) and later put into the G-code buffer of the Queue channel by QueuedCode::AssignTo (GCodeQueue::FillBuffer,
// GCodeQueue.cpp:140-158). In SBC mode the code is binary and AssignTo hands it to GCodeBuffer::PutBinary, which hands it to
// BinaryParser::Put. BinaryParser::Put no longer allocates the buffer itself (upstream e520116e9): SbcInterface::FillBuffer calls
// GCodeBuffer::AllocateBinaryBuffer before the first code of a channel (src/SBC/SbcInterface.cpp:2301-2306 and 2381-2384). The queue
// G-code buffer gets its codes from GCodeQueue::FillBuffer (src/GCodes/GCodes.cpp:744), not from the SBC code ring, so it had no buffer,
// bufferLength was 0 (GCodeBuffer.cpp:117), BinaryParser::Put set overflowed (src/GCodes/GCodeBuffer/BinaryParser.cpp:43-48) and
// GCodes::ActOnCode threw "GCode command too long" (src/GCodes/GCodes2.cpp:125-128). Since upstream 187840328 AssignTo allocates the
// buffer before PutBinary if the G-code buffer has none.
//
// The test plays QueuedCode with the members that AssignTo uses, and GCodeBuffer with a stand-in whose PutBinary stores the code as
// BinaryParser::Put does and overflows like it when the code is longer than the buffer, which it is for any code while there is no buffer.
// It checks that a binary code put into a G-code buffer without a buffer gets one before PutBinary and is stored whole, that a G-code
// buffer with a buffer is not allocated again, so that consecutive codes share one, that the fork's channel epoch 0 reaches PutBinary,
// and that a text code is put with PutAndDecode without allocating a binary buffer.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define HAS_SBC_INTERFACE 1

constexpr size_t MaxGCodeBinaryLength = 384;					// src/SBC/SbcMessageFormats.h:31
constexpr uint8_t ChannelEpochValid = 0x80;						// src/SBC/SbcMessageFormats.h:115
constexpr size_t ShortGCodeLength = 64;							// src/Config/Configuration.h:172
constexpr size_t BufferSizePerQueueItem = ShortGCodeLength;		// src/GCodes/GCodeQueue.h:15

static std::vector<std::string> failures;
static unsigned int allocations = 0;

// Just what AssignTo uses of GCodeBuffer (src/GCodes/GCodeBuffer/GCodeBuffer.h), with the buffer state that BinaryParser::Put reads and
// writes. The constructor leaves the buffer unallocated with a length of 0, as GCodeBuffer::GCodeBuffer does (GCodeBuffer.cpp:117).
class GCodeBuffer
{
public:
	~GCodeBuffer() noexcept
	{
		for (char *p : allocated)
		{
			free(p);
		}
	}

	// GCodeBuffer.h:278
	bool HasBuffer() const noexcept { return buffer != nullptr; }

	// As GCodeBuffer::AllocateBinaryBuffer (GCodeBuffer.cpp:334-338): MaxGCodeBinaryLength bytes from Tasks::AllocPermanent, which can never
	// be freed (src/Platform/Tasks.cpp:321-326), so the test keeps every allocation until the end
	void AllocateBinaryBuffer() noexcept
	{
		buffer = static_cast<char *>(malloc(MaxGCodeBinaryLength));
		bufferLength = MaxGCodeBinaryLength;
		allocated.push_back(buffer);
		++allocations;
	}

	// As GCodeBuffer::PutBinary (GCodeBuffer.cpp:322-332), which keeps the epoch on the latest level (codeEpochValid is set from
	// ChannelEpochValid, line 326), sets isBinaryBuffer (line 329) and calls BinaryParser::Put. Of that (BinaryParser.cpp:33-76) this plays
	// the length check and the copy: a code longer than gb.bufferLength sets overflowed and is not stored (lines 43-50), else memcpyu32
	// copies it word by word (line 53, memcpyu32 in CoreN2G src/CoreIO.cpp:633) and overflowed is cleared (line 54). The parameter check
	// (ParametersFit) and the line number and G53 flags (lines 55-75) are not played, the test codes are not parsed.
	void PutBinary(const uint32_t *data, size_t len, uint8_t epoch) noexcept
	{
		++putBinaryCalls;
		lastEpoch = epoch;
		hadBufferAtPut = HasBuffer();
		isBinaryBuffer = true;

		const size_t codeLength = len * sizeof(uint32_t);
		if (codeLength > bufferLength)
		{
			overflowed = true;
			storedLength = 0;
		}
		else if (buffer == nullptr)
		{
			// Cannot happen, bufferLength is 0 without a buffer. The real Put would go on to read the header through a null pointer
			failures.push_back("a code was put into a G-code buffer without a buffer");
		}
		else
		{
			uint32_t *const dst = reinterpret_cast<uint32_t *>(buffer);
			for (size_t i = 0; i < len; ++i)
			{
				dst[i] = data[i];
			}
			overflowed = false;
			storedLength = codeLength;
		}
	}

	// As GCodeBuffer::PutAndDecode(const char *, size_t) (GCodeBuffer.cpp:401-409), which clears isBinaryBuffer and hands the text to
	// StringParser::PutAndDecode. The string parser is not played: it allocates a buffer of its own in StringParser::Put
	// (src/GCodes/GCodeBuffer/StringParser.cpp:93-104), not with AllocateBinaryBuffer
	void PutAndDecode(const char *data, size_t len) noexcept
	{
		++putAndDecodeCalls;
		isBinaryBuffer = false;
		text.assign(data, len);
	}

	const char *Buffer() const noexcept { return buffer; }

	unsigned int putBinaryCalls = 0, putAndDecodeCalls = 0;
	uint8_t lastEpoch = 0xFF;
	bool hadBufferAtPut = false;
	bool isBinaryBuffer = false;
	bool overflowed = false;
	size_t storedLength = 0;
	std::string text;

private:
	char *buffer = nullptr;
	size_t bufferLength = 0;
	std::vector<char *> allocated;
};

// The members of QueuedCode in src/GCodes/GCodeQueue.h:42-65 that AssignTo uses. Fill stores a code as QueuedCode::AssignFrom does
// (GCodeQueue.cpp:246-253). Codes longer than BufferSizePerQueueItem are not queued (GCodeQueue.cpp:31 and 95)
class QueuedCode
{
public:
	void AssignTo(GCodeBuffer *gb) noexcept;

	void Fill(bool binary, const void *code, size_t length) noexcept
	{
		isBinary = binary;
		dataLength = (length < sizeof(data)) ? length : sizeof(data);
		memcpy(data, code, dataLength);
	}

	const char *Data() const noexcept { return data; }

private:
	bool isBinary = false;
	alignas(4) char data[BufferSizePerQueueItem];
	size_t dataLength = 0;
};

#include "AssignTo.inc"

static unsigned int passed = 0, failed = 0;

static void Add(std::string& why, const std::string& what) noexcept
{
	why += (why.empty() ? "" : "; ") + what;
}

static void Report(const char *name, std::string why) noexcept
{
	for (const std::string& f : failures)
	{
		Add(why, f);
	}
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
	failures.clear();
	allocations = 0;
}

// A binary code of the given length with a different byte at each position, so that a partial or misplaced copy shows. The length is a
// whole number of words, as AssignTo puts dataLength / 4 words
static std::vector<uint32_t> BinaryCode(size_t length, uint8_t seed) noexcept
{
	std::vector<uint32_t> code(length / sizeof(uint32_t));
	unsigned char *const bytes = reinterpret_cast<unsigned char *>(code.data());
	for (size_t i = 0; i < length; ++i)
	{
		bytes[i] = static_cast<unsigned char>(seed + 1 + i);
	}
	return code;
}

// Assign a binary queued code of the given length to the G-code buffer and say what is wrong with how it was stored
static std::string AssignBinary(GCodeBuffer& gb, size_t length, uint8_t seed) noexcept
{
	const std::vector<uint32_t> code = BinaryCode(length, seed);
	QueuedCode qc;
	qc.Fill(true, code.data(), length);
	const unsigned int putsBefore = gb.putBinaryCalls;
	qc.AssignTo(&gb);

	std::string why;
	if (gb.putBinaryCalls != putsBefore + 1)
	{
		return "PutBinary was not called once";
	}
	if (!gb.hadBufferAtPut)
	{
		Add(why, "PutBinary was called without a buffer");
	}
	if (gb.overflowed)
	{
		Add(why, "the code overflowed, which GCodes::ActOnCode reports as \"GCode command too long\"");
	}
	else if (gb.storedLength != length || gb.Buffer() == nullptr || memcmp(gb.Buffer(), qc.Data(), length) != 0)
	{
		Add(why, "the code was not stored whole");
	}
	if (!gb.isBinaryBuffer)
	{
		Add(why, "the buffer was not marked binary");
	}
	return why;
}

int main()
{
	{
		GCodeBuffer gb;
		std::string why = AssignBinary(gb, BufferSizePerQueueItem, 0);
		if (allocations != 1) { Add(why, "allocated " + std::to_string(allocations) + " buffers instead of 1"); }
		if (!gb.HasBuffer()) { Add(why, "the G-code buffer has no buffer"); }
		Report("a binary queued code put into a G-code buffer without a buffer gets one before PutBinary and is stored whole", why);
	}

	{
		GCodeBuffer gb;
		gb.AllocateBinaryBuffer();
		const char *const before = gb.Buffer();
		allocations = 0;
		std::string why = AssignBinary(gb, 36, 50);
		if (allocations != 0) { Add(why, "allocated the buffer again"); }
		if (gb.Buffer() != before) { Add(why, "the buffer was replaced"); }
		Report("a G-code buffer that has a buffer is not allocated again", why);
	}

	{
		GCodeBuffer gb;
		std::string why = AssignBinary(gb, 36, 100);
		const char *const first = gb.Buffer();
		const std::string second = AssignBinary(gb, BufferSizePerQueueItem, 150);
		if (!second.empty()) { Add(why, "second code: " + second); }
		const std::string third = AssignBinary(gb, 8, 200);
		if (!third.empty()) { Add(why, "third code: " + third); }
		if (allocations != 1) { Add(why, "allocated " + std::to_string(allocations) + " buffers instead of 1"); }
		if (first == nullptr || gb.Buffer() != first) { Add(why, "the codes did not share the first buffer"); }
		Report("consecutive binary queued codes in one G-code buffer share the buffer allocated for the first", why);
	}

	{
		// With a buffer already there, so that this checks the epoch alone
		GCodeBuffer gb;
		gb.AllocateBinaryBuffer();
		allocations = 0;
		std::string why = AssignBinary(gb, 36, 10);
		if (gb.lastEpoch != 0) { Add(why, "PutBinary got epoch " + std::to_string(gb.lastEpoch) + " instead of 0"); }
		if ((gb.lastEpoch & ChannelEpochValid) != 0) { Add(why, "the epoch is marked valid"); }
		Report("a binary queued code is put with channel epoch 0, which marks it as having no epoch", why);
	}

	{
		GCodeBuffer gb;
		static const char line[] = "M106 P0 S0.5";
		QueuedCode qc;
		qc.Fill(false, line, strlen(line));
		qc.AssignTo(&gb);
		std::string why;
		if (gb.putAndDecodeCalls != 1 || gb.text != line) { Add(why, "the text was not put with PutAndDecode"); }
		if (gb.putBinaryCalls != 0) { Add(why, "PutBinary was called"); }
		if (allocations != 0) { Add(why, "a binary buffer was allocated"); }
		if (gb.isBinaryBuffer) { Add(why, "the buffer is marked binary"); }
		Report("a text queued code is put with PutAndDecode and allocates no binary buffer", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
