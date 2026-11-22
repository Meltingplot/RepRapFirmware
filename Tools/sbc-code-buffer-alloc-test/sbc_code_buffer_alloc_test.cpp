// Host test of the first binary code of a G-code buffer in SBC mode (SbcInterface::FillBuffer in src/SBC/SbcInterface.cpp), compiled
// from the real text of the function that the Makefile extracts.
//
// FillBuffer runs in the main task. It walks the codes that the SBC task has stored inside a TaskCriticalSectionLocker, which
// suspends the scheduler, and hands the next code of the channel to GCodeBuffer::PutBinary. BinaryParser::Put
// (src/GCodes/GCodeBuffer/BinaryParser.cpp) allocates the buffer of a G-code buffer with Tasks::AllocPermanent when the buffer gets its
// first code. AllocPermanent takes the malloc mutex only while the scheduler is running (GetMallocMutex in src/Platform/Tasks.cpp), so
// with the scheduler suspended it runs without it, while a task of the same or a lower priority may have been preempted inside malloc.
// Up to 3.7-dev de1bfe598 the first code of each channel was put, and its buffer allocated, inside the locker.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter, and GCodeBuffer with a stand-in whose PutBinary
// allocates the buffer when there is none, as BinaryParser::Put does. No allocation may happen with the scheduler suspended. It also
// checks what FillBuffer does: the call that finds the first code of a channel without a buffer primes the buffer and leaves the code
// pending, the next call reads it, codes of other channels stay pending and are read once, the ring pointers are reset when the buffer
// is read completely, and nothing is read or allocated while no code is pending.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define RRF_ASSERT(_expr) do { if (!(_expr)) { printf("RRF_ASSERT failed at %s:%d\n", __FILE__, __LINE__); exit(2); } } while (false)

constexpr uint16_t SbcCodeBufferSize = 4096;					// src/SBC/SbcMessageFormats.h
constexpr size_t MaxGCodeBinaryLength = 384;					// src/SBC/SbcMessageFormats.h

static unsigned int suspended = 0;							// uxSchedulerSuspended
static std::vector<std::string> failures;
static unsigned int allocations = 0;

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

namespace Tasks
{
	void *AllocPermanent(size_t sz) noexcept
	{
		if (suspended != 0)
		{
			failures.push_back("Tasks::AllocPermanent was called with the scheduler suspended");
		}
		++allocations;
		return malloc(sz);
	}
}

// The headers in src/SBC/SbcMessageFormats.h that FillBuffer reads
struct BufferedCodeHeader
{
	bool isPending;
	uint8_t codeEpoch;
	uint16_t length;
};

struct CodeHeader
{
	uint8_t channel;
	uint8_t flags;
	uint8_t numParameters;
	char letter;
	int32_t majorCode;
	int32_t minorCode;
	uint32_t filePosition;
	int32_t lineNumber;
};

// Just what FillBuffer uses of GCodeBuffer (src/GCodes/GCodeBuffer/GCodeBuffer.h). PutBinary allocates the buffer when there is none,
// as BinaryParser::Put does, and PrimeBinaryBuffer allocates it in the same way.
struct MachineState
{
	bool waitingForAcknowledgement = false;
};

class Channel
{
public:
	explicit Channel(uint8_t c) noexcept : channel(c) { }
	uint8_t RawValue() const noexcept { return channel; }

private:
	uint8_t channel;
};

class GCodeBuffer
{
public:
	explicit GCodeBuffer(uint8_t c) noexcept : channel(c) { }
	~GCodeBuffer() noexcept { free(buffer); }

	bool IsInvalidated() const noexcept { return false; }
	bool IsMacroFileClosed() const noexcept { return false; }
	bool IsMessageAcknowledged() const noexcept { return false; }
	bool IsAbortRequested() const noexcept { return false; }
	bool IsFileChannel() const noexcept { return false; }
	bool IsMessagePromptPending() const noexcept { return false; }
	bool IsRecordPending() const noexcept { return false; }
	MachineState& LatestMachineState() noexcept { return machineState; }
	Channel GetChannel() const noexcept { return channel; }

	bool HasBuffer() const noexcept { return buffer != nullptr; }
	void PrimeBinaryBuffer() noexcept { if (buffer == nullptr) { buffer = static_cast<char *>(Tasks::AllocPermanent(MaxGCodeBinaryLength)); } }

	void PutBinary(const uint32_t *data, size_t len, uint8_t epoch) noexcept
	{
		(void)epoch;
		PrimeBinaryBuffer();
		memcpy(buffer, data, len * sizeof(uint32_t));
		lastMajorCode = reinterpret_cast<const CodeHeader *>(data)->majorCode;
		++codesPut;
	}

	void DecodeCommand() noexcept { }

	unsigned int codesPut = 0;
	int32_t lastMajorCode = -1;

private:
	Channel channel;
	MachineState machineState;
	char *buffer = nullptr;
};

// The members of SbcInterface in src/SBC/SbcInterface.h that FillBuffer uses, and a stand-in of the SBC task storing a code
class SbcInterface
{
public:
	bool FillBuffer(GCodeBuffer &gb) noexcept;

	void Store(uint8_t channel, int32_t majorCode) noexcept
	{
		BufferedCodeHeader *const bh = reinterpret_cast<BufferedCodeHeader *>(codeBuffer + txPointer);
		bh->isPending = true;
		bh->codeEpoch = 0;
		bh->length = sizeof(CodeHeader);
		CodeHeader *const ch = reinterpret_cast<CodeHeader *>(codeBuffer + txPointer + sizeof(BufferedCodeHeader));
		memset(ch, 0, sizeof(CodeHeader));
		ch->channel = channel;
		ch->letter = 'G';
		ch->majorCode = majorCode;
		txPointer += sizeof(BufferedCodeHeader) + sizeof(CodeHeader);
	}

	alignas(4) char codeBuffer[SbcCodeBufferSize];
	volatile uint16_t rxPointer = 0, txPointer = 0, txEnd = 0;
	volatile bool sendBufferUpdate = false;
	bool reportPause = false;
};

#include "FillBuffer.inc"

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, std::string why) noexcept
{
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	if (suspended != 0)
	{
		why += (why.empty() ? "" : "; ") + std::string("the scheduler was left suspended");
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

// Read the first code of a channel without a buffer: the first call primes the buffer and must not read the code, the second call
// reads it. Return true if it went that way
static bool ReadOnSecondCall(SbcInterface& sbc, GCodeBuffer& gb) noexcept
{
	return !sbc.FillBuffer(gb) && gb.HasBuffer() && sbc.FillBuffer(gb);
}

int main()
{
	{
		SbcInterface sbc;
		GCodeBuffer gb(1);
		sbc.Store(1, 28);
		std::string why;
		if (sbc.FillBuffer(gb) || gb.codesPut != 0) { why = "the first call read the code"; }
		if (!gb.HasBuffer() || allocations != 1) { why += " the first call did not prime the buffer"; }
		if (sbc.rxPointer != 0 || sbc.txPointer == 0 || sbc.sendBufferUpdate) { why += " the first call moved the ring pointers"; }
		if (!sbc.FillBuffer(gb) || gb.codesPut != 1 || gb.lastMajorCode != 28) { why += " the second call did not read the code"; }
		if (allocations != 1) { why += " the buffer was allocated " + std::to_string(allocations) + " times"; }
		if (sbc.rxPointer != 0 || sbc.txPointer != 0 || !sbc.sendBufferUpdate) { why += " the ring pointers were not reset"; }
		Report("the first code of a channel primes its buffer with the scheduler running and is read on the next call", why);
	}

	{
		SbcInterface sbc;
		GCodeBuffer gb(1);
		sbc.Store(1, 1);
		(void)ReadOnSecondCall(sbc, gb);
		allocations = 0;
		sbc.Store(1, 2);
		std::string why;
		if (!sbc.FillBuffer(gb) || gb.lastMajorCode != 2) { why = "the second code was not read by the first call"; }
		if (allocations != 0) { why += " the buffer was allocated again"; }
		Report("a channel with a buffer reads its next code at once", why);
	}

	{
		SbcInterface sbc;
		GCodeBuffer gb1(1), gb2(2);
		sbc.Store(1, 10);
		sbc.Store(2, 20);
		std::string why;
		if (!ReadOnSecondCall(sbc, gb2) || gb2.lastMajorCode != 20) { why = "the code of channel 2 was not read on the second call"; }
		if (gb1.codesPut != 0) { why += " channel 1 got a code"; }
		if (!ReadOnSecondCall(sbc, gb1) || gb1.lastMajorCode != 10) { why += " the code of channel 1 was not read on the second call afterwards"; }
		if (allocations != 2) { why += " not two buffers allocated"; }
		if (sbc.FillBuffer(gb1) || sbc.FillBuffer(gb2)) { why += " a code was read twice"; }
		Report("the code of another channel stays pending until its channel reads it", why);
	}

	{
		SbcInterface sbc;
		GCodeBuffer gb(1);
		std::string why;
		if (sbc.FillBuffer(gb) || sbc.FillBuffer(gb)) { why = "a code was read"; }
		if (allocations != 0) { why += " a buffer was allocated"; }
		Report("without pending codes nothing is read or allocated", why);
	}

	{
		SbcInterface sbc;
		GCodeBuffer gb1(1), gb3(3);
		sbc.Store(3, 30);
		std::string why;
		if (sbc.FillBuffer(gb1) || sbc.FillBuffer(gb1)) { why = "channel 1 read the code of channel 3"; }
		if (allocations != 0) { why += " channel 1 allocated a buffer for a code of another channel"; }
		Report("a channel without codes allocates nothing while other channels have codes", why);
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
