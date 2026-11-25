// Fuzz target for the packets that DSF sends to RRF in SBC mode, after the transfer has arrived whole (SPI: after the header and data
// CRCs have passed; USB: after DoTransferUsb has read the header and the data).
//
// Real code:
// - src/SBC/DataTransfer.cpp, compiled unchanged (build/*/DataTransfer.o): ReadPacket, ReadData and every Read* function, the Write*
//   functions of the replies, DoTransferUsb and SwitchToUsb
// - the packet loop of SbcInterface::ExchangeData, SbcInterface::IsDroppedByEpoch, RecordChannelAbort, InvalidateBufferedCodes and
//   FillBuffer, copied out of src/SBC/SbcInterface.cpp at build time (gen/SbcPackets.inc)
// - OutputBuffer::cat, copy, Clear and Length, copied out of src/Platform/OutputMemory.cpp (gen/OutputBuffer.inc)
// - StringRef, String<N> and the string functions of RRFLibraries, Storage/CRC32.cpp, GCodes/GCodeFileInfo.cpp, compiled unchanged
//
// Played here (not RRF): the G-code buffers, GCodes, the variables and the expression parser, the object model, PrintMonitor,
// SoftwareReset and the IAP start. See README.md for what each of them does.
//
// Input: byte 0 selects the transport and the file operation that waits for a result, byte 1 the state of the G-code buffers, byte 2
// the size of the read buffer of that file operation. Then the transfers:
// - SPI: per transfer one byte numPackets and a 16-bit dataLength (little endian), then dataLength bytes of data, or fewer if the input
//   ends before. They become rxHeader and the content of rxBuffer as DoTransfer leaves them after a transfer whose CRCs passed.
// - USB: the bytes that DSF writes, as they are: an 8-byte UsbTransferHeader, then its data, as many transfers as the input holds.
// After each transfer the extracted packet loop runs (the SBC task), then FillBuffer for every channel (the main task).

#include <csetjmp>
#include <cstdlib>
#include <vector>

#include <FuzzShim.h>
#include <GCodes/GCodeChannel.h>
#include <GCodes/GCodeFileInfo.h>
#include <SBC/SbcMessageFormats.h>
#define private public				// the target sets rxHeader, rxBuffer and rxPointer as DoTransfer does after a good transfer
#include <SBC/DataTransfer.h>
#undef private
#if defined(__SANITIZE_ADDRESS__)
# define FUZZ_ASAN	1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define FUZZ_ASAN	1
# endif
#endif
#ifdef FUZZ_ASAN
# include <sanitizer/asan_interface.h>
# include <sanitizer/common_interface_defs.h>
#endif

// ---------------------------------------------------------------------------------------------------------------------
// OutputBuffer: the real cat, copy, Clear and Length, and a pool of OUTPUT_BUFFER_COUNT buffers

#include "OutputBuffer.inc"

namespace
{
	OutputBuffer *outputPool[OUTPUT_BUFFER_COUNT];
	bool outputUsed[OUTPUT_BUFFER_COUNT];
}

bool OutputBuffer::Allocate(OutputBuffer *&buf, bool) noexcept
{
	for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
	{
		if (!outputUsed[i])
		{
			outputUsed[i] = true;
			buf = outputPool[i];
			buf->next = nullptr;
			buf->last = buf;
			buf->references = 1;
			buf->dataLength = buf->bytesRead = 0;
			buf->isReferenced = buf->hadOverflow = false;
			return true;
		}
	}
	buf = nullptr;
	return false;
}

OutputBuffer *OutputBuffer::Release(OutputBuffer *buf) noexcept
{
	OutputBuffer *const next = buf->next;
	for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
	{
		if (outputPool[i] == buf)
		{
			outputUsed[i] = false;
		}
	}
	return next;
}

void OutputBuffer::ReleaseAll(OutputBuffer *&buf) noexcept
{
	while (buf != nullptr)
	{
		buf = Release(buf);
	}
}

unsigned int OutputBuffer::GetFreeBuffers() noexcept
{
	unsigned int n = 0;
	for (bool used : outputUsed) { if (!used) { ++n; } }
	return n;
}

void OutputBuffer::ResetPool() noexcept
{
	for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
	{
		if (outputPool[i] == nullptr)
		{
			outputPool[i] = new OutputBuffer(nullptr);		// one heap block each, so a write beyond data[] is seen
		}
		outputUsed[i] = false;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Played parts of RRF

namespace
{
	jmp_buf endOfInput;				// SoftwareReset, StartIap and ReceiveAndStartIap do not return on the board
	unsigned int internalErrors = 0;
	uint32_t virtualTime = 0;
	bool verbose = false;

	// State of every G-code buffer, from byte 1 of the input
	enum GbFlags : uint8_t
	{
		gbWaitingForMacro = 1, gbRecordPending = 2, gbExecutingOnSbc = 4, gbMutexFails = 8, gbInvalidated = 16, gbMacroStartedByCode = 32,
		gbLockFails = 64, gbNoBuffer = 128
	};
	uint8_t gbFlags = 0;
}

Spi FuzzSpi;
Xdmac FuzzXdmac;
void ESP_SPI_HANDLER() noexcept { }			// SPI1_Handler in DataTransfer.cpp passes the interrupt on to it
RepRap reprap;

void FuzzInternalError(const char *file, int line) noexcept
{
	++internalErrors;
	if (verbose) { fprintf(stderr, "internal error at %s:%d\n", file, line); }
}

extern "C" void debugPrintf(const char *fmt, ...) noexcept
{
	if (!verbose) { return; }
	va_list vargs;
	va_start(vargs, fmt);
	vfprintf(stderr, fmt, vargs);
	va_end(vargs);
}

[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept
{
	fprintf(stderr, "RRF_ASSERT failed at %s:%u, the board would reset\nSUMMARY: RRF_ASSERT %s:%u\n", file, (unsigned int)line, file, (unsigned int)line);
	abort();
}

uint32_t millis() noexcept { return virtualTime; }
void delay(uint32_t ms) noexcept { virtualTime += ms; }

void TaskBase::GiveFromISR(TaskBase *, uint32_t) noexcept { }
bool TaskBase::TakeIndexed(uint32_t, uint32_t) noexcept { return true; }
TaskBase *TaskBase::GetCallerTaskHandle() noexcept { return nullptr; }
MutexLocker::MutexLocker(Mutex&, uint32_t) noexcept : acquired((gbFlags & gbMutexFails) == 0) { }

void InitAllocationFromPbufPool() noexcept { }
void *AllocateFromPbufPool(size_t bytes) noexcept
{
	return calloc(1, bytes);			// a heap block each, see FuzzShim.h; the target allocates them once
}

bool RepRap::Debug(Module) const noexcept { return verbose; }
void RepRap::EmergencyStop() noexcept { }
void RepRap::StartIap(const char *) noexcept { longjmp(endOfInput, 1); }

void Platform::Message(MessageType, const char *msg) noexcept { (void)strlen(msg); }
void Platform::Message(MessageType, OutputBuffer *buf) noexcept { OutputBuffer::ReleaseAll(buf); }
void Platform::MessageF(MessageType, const char *fmt, ...) noexcept
{
	char buf[256];
	va_list vargs;
	va_start(vargs, fmt);
	vsnprintf(buf, sizeof(buf), fmt, vargs);
	va_end(vargs);
}
void Platform::ReinitUsbDevice(unsigned int) noexcept { }
void Platform::DisconnectUsb() noexcept { }

// The USB device: DSF's bytes from the fuzz input. A read that the input cannot fill returns what is left, as a timeout would
size_t SerialCDC::readDirect(uint8_t *dst, size_t len, uint32_t) noexcept
{
	const size_t n = std::min(len, length - position);
	memcpy(dst, data + position, n);
	position += n;
	return n;
}
bool SerialCDC::writeDirect(const uint8_t *src, size_t len, uint32_t) noexcept
{
	volatile uint8_t sum = 0;
	for (size_t i = 0; i < len; ++i) { sum += src[i]; }		// DSF reads what RRF sends
	return true;
}

enum class SoftwareResetReason : uint16_t { user, userFromSbc };
static void SoftwareReset(SoftwareResetReason) noexcept { longjmp(endOfInput, 1); }

// GCodes/GCodeMachineState.h, the members that BinaryParser::Put and FillBuffer use
class GCodeMachineState
{
public:
	bool waitingForAcknowledgement = false;
};

// GCodes/GCodeBuffer/GCodeBuffer.h: the members that the extracted text uses. The epoch members do what the real ones do, the others
// return the state from byte 1 of the input. PutBinary copies the code as BinaryParser::Put does (the Tools/binary-code-fuzz target
// fuzzes the parser itself). A GB without gbNoBuffer has its buffer from the start; the stand-in allocates it at its first code
class GCodeBuffer
{
public:
	explicit GCodeBuffer(GCodeChannel c) noexcept : channel(c) { }

	GCodeChannel GetChannel() const noexcept { return channel; }
	bool IsFileChannel() const noexcept { return channel == GCodeChannel::File || channel == GCodeChannel::File2; }

	void ResetChannelEpoch(bool known) noexcept { channelEpoch = 0; channelEpochKnown = known; abortGateSet = false; }
	bool IsChannelEpochKnown() const noexcept { return channelEpochKnown; }
	uint8_t GetChannelEpoch() const noexcept { return channelEpoch; }
	void SetChannelEpoch(uint8_t e) noexcept { channelEpoch = e & ChannelEpochMask; channelEpochKnown = true; }
	bool IsAbortGateSet() const noexcept { return abortGateSet; }
	void SetAbortGate(bool b) noexcept { abortGateSet = b; }

	bool IsInvalidated() const noexcept { return (gbFlags & gbInvalidated) != 0 || invalidated; }
	void Invalidate(bool) noexcept { invalidated = true; }
	bool IsWaitingForMacro() const noexcept { return (gbFlags & gbWaitingForMacro) != 0; }
	bool IsMacroRequestPending() const noexcept { return false; }
	void ResolveMacroRequest(bool, bool) noexcept { }
	void MacroRequestSent() noexcept { }
	bool IsMacroStartedByCode() const noexcept { return (gbFlags & gbMacroStartedByCode) != 0; }
	void MarkReplyVoid() noexcept { }
	bool IsRecordPending() const noexcept { return (gbFlags & gbRecordPending) != 0; }
	void SetAbortRecorded(bool) noexcept { }
	void SetConnectionEndRecorded() noexcept { }
	void SetFailedMacroPending(bool) noexcept { }
	bool IsFailedMacroPending() const noexcept { return false; }
	void SetFileFinished() noexcept { }
	void SetPrintFinished() noexcept { }
	bool IsExecutingOnSbc() const noexcept { return (gbFlags & gbExecutingOnSbc) != 0; }
	void SetFinished(bool) noexcept { }
	void SetLastResult(GCodeResult r) noexcept { lastResult = r; }
	VariableSet& GetVariables() noexcept;

	// FillBuffer
	bool IsMacroFileClosed() const noexcept { return false; }
	bool IsMessageAcknowledged() const noexcept { return false; }
	bool IsAbortRequested() const noexcept { return false; }
	bool IsMessagePromptPending() const noexcept { return false; }
	GCodeMachineState& LatestMachineState() noexcept { return machineState; }
	bool HasBuffer() const noexcept { return (gbFlags & gbNoBuffer) == 0 || buffer != nullptr; }
	void AllocateBinaryBuffer() noexcept { if (buffer == nullptr) { buffer = new uint32_t[MaxGCodeBinaryLength / sizeof(uint32_t)]; } }
	void PutBinary(const uint32_t *data, size_t len, uint8_t) noexcept
	{
		AllocateBinaryBuffer();
		if (len * sizeof(uint32_t) <= MaxGCodeBinaryLength)		// BinaryParser::Put
		{
			memcpyu32(buffer, data, len);
			++codesPut;
		}
		else
		{
			++codesOverflowed;
		}
	}
	void DecodeCommand() noexcept { }

	Mutex mutex;
	GCodeChannel channel;
	uint8_t channelEpoch = 0;
	bool channelEpochKnown = false, abortGateSet = false, invalidated = false;
	GCodeResult lastResult = GCodeResult::ok;
	GCodeMachineState machineState;
	uint32_t *buffer = nullptr;
	unsigned int codesPut = 0, codesOverflowed = 0;
};

class GCodes
{
public:
	GCodeBuffer *GetGCodeBuffer(GCodeChannel c) noexcept { return gbs[c.ToBaseType()]; }
	bool LockAllMovementSystemsAndWaitForStandstill(GCodeBuffer&) noexcept { return (gbFlags & gbLockFails) == 0; }
	void UnlockAll(GCodeBuffer&) noexcept { }
	void ChannelAbortRecorded() noexcept { }

	GCodeBuffer *gbs[NumGCodeChannels];
};

class PrintMonitor
{
public:
	void SetPrintingFileInfo(const char *filename, GCodeFileInfo& info) noexcept
	{
		(void)strlen(filename);
		(void)info.generatedBy.strlen();
		float sum = 0.0;
		for (size_t i = 0; i < info.numFilaments; ++i) { sum += info.filamentNeeded[i]; }	// the real one reports numFilaments of them
		(void)sum;
	}
};

// ObjectModel/Variable.h and the object model: names are read as C strings, as the real classes do
class Variable
{
public:
	void Assign(const ExpressionValue&) noexcept { }
	void AssignIndexed(const ExpressionValue&, size_t numIndices, const uint32_t *indices) THROWS(GCodeException)
	{
		for (size_t i = 0; i < numIndices; ++i) { if (indices[i] > 1000) { throw GCodeException("array index out of range"); } }
	}
};

class VariableSet
{
public:
	Variable *Lookup(const char *name, bool) noexcept { return (strlen(name) % 2 == 0) ? &var : nullptr; }
	void InsertNew(const char *name, const ExpressionValue&, int) THROWS(GCodeException) { (void)strlen(name); }
	void Delete(const char *name) noexcept { (void)strlen(name); }
	Variable var;
};

VariableSet& GCodeBuffer::GetVariables() noexcept { static VariableSet localVars; return localVars; }

template<class T> class WriteLockedPointer
{
public:
	WriteLockedPointer(void *, T *p) noexcept : ptr(p) { }
	T *operator->() const noexcept { return ptr; }
	T *Ptr() const noexcept { return ptr; }
private:
	T *ptr;
};

namespace
{
	GCodes gcodes;
	PrintMonitor printMonitor;
	VariableSet globalVariables;
}

GCodes& RepRap::GetGCodes() noexcept { return gcodes; }
PrintMonitor& RepRap::GetPrintMonitor() noexcept { return printMonitor; }
WriteLockedPointer<VariableSet> RepRap::GetGlobalVariablesForWriting() noexcept { return WriteLockedPointer<VariableSet>(nullptr, &globalVariables); }

OutputBuffer *RepRap::GetModelResponse(const void *, const char *key, const char *flags) THROWS(GCodeException)
{
	if (key[0] == 'E') { throw GCodeException("unknown key"); }
	OutputBuffer *buf;
	if (!OutputBuffer::Allocate(buf)) { return nullptr; }
	buf->cat("{\"key\":\"");
	buf->cat(key);
	buf->cat("\",\"flags\":\"");
	buf->cat(flags);
	buf->cat("\"}");
	return buf;
}

struct ObjectExplorationContext { void AddIndex(int32_t) noexcept { } };
class ObjectModel
{
public:
	void ReportItemAsJsonFull(OutputBuffer *buf, ObjectExplorationContext&, const void *, const ExpressionValue&, const char *) const THROWS(GCodeException) { buf->cat("[]"); }
	static ReadWriteLock lifetimeLock;
};
ReadWriteLock ObjectModel::lifetimeLock;
static void ReportHeapArrayAsJson(OutputBuffer *buf, ObjectExplorationContext&, const void *, const FuzzArrayHandle&, const char *) THROWS(GCodeException) { buf->cat("[]"); }

// GCodes/GCodeBuffer/ExpressionParser.h. The real parser reads the expression as a C string up to its end; this one does the same and
// returns a value chosen by the first character. The index parser of SetVariable reads digits up to the end it is given
class ExpressionParser
{
public:
	ExpressionParser(GCodeBuffer *, const char *text) noexcept : p(text), end(text + strlen(text)) { }
	ExpressionParser(GCodeBuffer *, const char *text, const char *textEnd, int = -1) noexcept : p(text), end(textEnd) { }

	ExpressionValue Parse() THROWS(GCodeException)
	{
		ExpressionValue v;
		const char first = (p < end) ? *p : 0;
		size_t n = 0;
		while (p + n < end && p[n] != 0) { ++n; }
		switch (first)
		{
		case 'E': throw GCodeException("fuzz parse error");
		case '"': v.type = (uint8_t)TypeCode::CString; v.sVal = p + 1; break;
		case 'f': v.type = (uint8_t)TypeCode::Float; v.fVal = 1.5f; v.param = 2; break;
		case 'u': v.type = (uint8_t)TypeCode::Uint64; break;
		case 'a': v.type = (uint8_t)TypeCode::HeapArray; break;
		case 'o': v.type = (uint8_t)TypeCode::ObjectModelArray; v.omVal = &om; break;
		case 'b': v.type = (uint8_t)TypeCode::Bool_tc; v.bVal = true; break;
		default: v.type = (uint8_t)TypeCode::Int32; v.iVal = (int32_t)n; break;
		}
		return v;
	}
	uint32_t ParseUnsigned() THROWS(GCodeException)
	{
		uint32_t v = 0;
		if (p >= end || *p < '0' || *p > '9') { throw GCodeException("expected unsigned integer"); }
		while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p++ - '0'); }
		return v;
	}
	const char *GetEndptr() const noexcept { return p; }
	void CheckForExtraCharacters() THROWS(GCodeException) { }

private:
	const char *p;
	const char *end;
	ObjectModel om;
};

// ---------------------------------------------------------------------------------------------------------------------
// SbcInterface: the members that the extracted text uses, as src/SBC/SbcInterface.h declares them

constexpr uint32_t SbcYieldTimeout = 10;			// SbcInterface.cpp
#include "SbcConstants.inc"						// MinimumBuffersForSbcJson, MaxSbcAnswerDataLength and ValueTooLongForSbc of SbcInterface.cpp

static void StopUsbTask() noexcept { }

class SbcInterface
{
public:
	SbcInterface() noexcept;
	void ProcessPackets() noexcept;									// the packet loop of ExchangeData
	bool FillBuffer(GCodeBuffer &gb) noexcept;
	bool IsDroppedByEpoch(GCodeBuffer& gb, const PacketHeader& packet) noexcept;
	bool MakeJsonAnswer(OutputBuffer *_ecv_null &json, const ExpressionValue& val, size_t otherDataLength) THROWS(GCodeException);
	void RecordChannelAbort(GCodeBuffer& gb, bool connectionLost) noexcept;
	void InvalidateBufferedCodes(GCodeChannel channel) noexcept;
	void EndConnection(bool waitForMainTask) noexcept;
	[[noreturn]] void ReceiveAndStartIap(const char *iapChunk, size_t length) noexcept;

	DataTransfer transfer;
	bool isConnected = true;
	uint32_t burstModeStartTime = 0;
	bool reportPause = false, printAborted = false, sbcPacketRead = false;
	uint32_t whenAbortRecorded = 0;
	uint32_t numStalePackets = 0, numAheadPackets = 0, numGatedPackets = 0, numRepeatedAborts = 0;
	enum class EpochDrop : uint8_t { none, stale, ahead, gated };
	uint16_t lastDropRequest = 0;
	uint8_t lastDropChannel = 0;
	EpochDrop lastDrop = EpochDrop::none;
	GCodeFileInfo fileInfo;

	char *codeBuffer;
	volatile uint16_t rxPointer = 0, txPointer = 0, txEnd = 0;
	volatile bool sendBufferUpdate = true;
	unsigned int numDisconnects = 0;

	unsigned int numOpenFiles = 0;
	BinarySemaphore fileSemaphore;
	enum class FileOperation {
		none, checkFileExists, deleteFileOrDirectory, deleteFileOrDirectoryRecursively, openRead, openWrite, openAppend, read, write,
		writeAsync, seek, truncate, close, secureDeleteFile, getFileList
	} fileOperation = FileOperation::none;
	std::atomic<bool> fileOperationPending{false};
	FileHandle fileHandle = noFileHandle;
	std::atomic<bool> fileSuccess{false};
	char *fileReadBuffer = nullptr;
	size_t fileBufferLength = 0;
	FilePosition fileOffset = 0;
	bool fileListEndOfList = false;
	size_t asyncWriteLength = 0;
	void FinishAsyncWrite(bool) noexcept { fileOperation = FileOperation::none; }
};

static SbcInterface *sbc = nullptr;
bool FuzzSbcConnection::IsConnected() const noexcept { return sbc != nullptr && sbc->isConnected; }

SbcInterface::SbcInterface() noexcept
{
	codeBuffer = (char *)new uint32_t[(SbcCodeBufferSize + 3)/4];	// SbcInterface::Init
	transfer.Init();
}

// The real EndConnection calls InvalidateResources, which drops the buffered codes, and waits for the main task
void SbcInterface::EndConnection(bool) noexcept
{
	numDisconnects++;
	rxPointer = txPointer = txEnd = 0;
}

// The real one copies the chunk to IAP_IMAGE_START and never returns. This one copies the chunk as the real one does, to a buffer of the
// size of the IAP region, and ends the input
[[noreturn]] void SbcInterface::ReceiveAndStartIap(const char *iapChunk, size_t length) noexcept
{
	static uint32_t iapImage[65536 / sizeof(uint32_t)];
	if (length <= sizeof(iapImage))
	{
		memcpyu32(iapImage, reinterpret_cast<const uint32_t *>(iapChunk), length / sizeof(uint32_t));
	}
	longjmp(endOfInput, 1);
}

void SbcInterface::ProcessPackets() noexcept
{
	bool codeBufferAvailable = true;			// ExchangeData, before the packet loop
#include "SbcPacketLoop.inc"
}

#include "SbcFunctions.inc"

// ---------------------------------------------------------------------------------------------------------------------
// Driver

namespace
{
	const size_t fileBufferSizes[] = { 0, 1, 7, 16, 64, 255, 256, 1024, 2048, 4096, 8191, 8192 };

	void StartInput(const uint8_t *data, size_t size) noexcept
	{
		const uint8_t config = (size > 0) ? data[0] : 0;
		gbFlags = (size > 1) ? data[1] : 0;
		const uint8_t bufferSize = (size > 2) ? data[2] : 0;

		if (sbc == nullptr)
		{
			OutputBuffer::ResetPool();
			for (size_t c = 0; c < NumGCodeChannels; ++c)
			{
				gcodes.gbs[c] = new GCodeBuffer(GCodeChannel(c));
			}
			sbc = new SbcInterface();
		}
		OutputBuffer::ResetPool();
		for (size_t c = 0; c < NumGCodeChannels; ++c)
		{
			GCodeBuffer& gb = *gcodes.gbs[c];
			gb.ResetChannelEpoch(false);
			gb.invalidated = false;
		}

		// A fresh connection, as after a connection reset
		SbcInterface& s = *sbc;
		s.rxPointer = s.txPointer = s.txEnd = 0;
		s.sbcPacketRead = false;
		s.printAborted = false;
		s.transfer.transportType = SbcTransportType::spi;
		s.transfer.activeLinkCapabilities = 0;
		s.transfer.rxPointer = s.transfer.txPointer = 0;
		s.transfer.packetId = 0;

		// The file operation that waits for its result, with a read buffer of its own size (ReadFile and GetFileList pass the caller's buffer)
		static const SbcInterface::FileOperation operations[] =
		{
			SbcInterface::FileOperation::none, SbcInterface::FileOperation::read, SbcInterface::FileOperation::getFileList,
			SbcInterface::FileOperation::write, SbcInterface::FileOperation::checkFileExists, SbcInterface::FileOperation::openRead,
			SbcInterface::FileOperation::writeAsync, SbcInterface::FileOperation::seek
		};
		s.fileOperation = operations[(config >> 1) & 7];
		static char *readBuffers[ARRAY_SIZE(fileBufferSizes)];
		const size_t sizeIndex = bufferSize % ARRAY_SIZE(fileBufferSizes);
		if (readBuffers[sizeIndex] == nullptr)
		{
			readBuffers[sizeIndex] = new char[std::max<size_t>(fileBufferSizes[sizeIndex], 1)];	// one heap block each, never freed
		}
		s.fileReadBuffer = readBuffers[sizeIndex];
		s.fileBufferLength = fileBufferSizes[sizeIndex];
		s.asyncWriteLength = (config & 0x80) ? 16 : 0;
	}

	// The main task: take the next code of every channel, as GCodes::SpinGCodeBuffer does through GCodeBuffer::... -> SbcInterface::FillBuffer
	void RunMainTask() noexcept
	{
		for (size_t c = 0; c < NumGCodeChannels; ++c)
		{
			while (sbc->FillBuffer(*gcodes.gbs[c])) { }
		}
	}

	// Poison the part of rxBuffer that this transfer did not fill, so that a read beyond the received data is reported
	void PoisonStale(size_t dataLength) noexcept
	{
#ifdef FUZZ_ASAN
		ASAN_UNPOISON_MEMORY_REGION(sbc->transfer.rxBuffer, SbcTransferBufferSize);
		if (getenv("FUZZ_POISON_STALE") != nullptr && dataLength < SbcTransferBufferSize)
		{
			ASAN_POISON_MEMORY_REGION(sbc->transfer.rxBuffer + dataLength, SbcTransferBufferSize - dataLength);
		}
#else
		(void)dataLength;
#endif
	}

	void RunSpi(const uint8_t *data, size_t size) noexcept
	{
		size_t pos = 3;
		while (pos + 3 <= size)
		{
			const uint8_t numPackets = data[pos];
			size_t dataLength = data[pos + 1] | (data[pos + 2] << 8);
			pos += 3;
			if (dataLength > SbcTransferBufferSize)
			{
				continue;							// DoTransfer answers BadDataLength, the data is not exchanged
			}
			dataLength = std::min(dataLength, size - pos);

			// What DoTransfer leaves after the header and data CRCs have passed: rxHeader as DSF sent it, the data in rxBuffer
			DataTransfer& t = sbc->transfer;
			t.rxHeader.formatCode = SbcFormatCode;
			t.rxHeader.numPackets = numPackets;
			t.rxHeader.protocolVersion = SbcProtocolVersion;
			t.rxHeader.dataLength = dataLength;
			memcpy(t.rxBuffer, data + pos, dataLength);
			pos += dataLength;
			t.rxPointer = t.txPointer = 0;
			t.packetId = 0;
			t.state = DataTransfer::InternalTransferState::ProcessingData;
			PoisonStale(dataLength);

			sbc->ProcessPackets();
			PoisonStale(SbcTransferBufferSize);
			RunMainTask();
		}
	}

	void RunUsb(const uint8_t *data, size_t size) noexcept
	{
		static SerialCDC device;
		device.data = data + 3;
		device.length = (size > 3) ? size - 3 : 0;
		device.position = 0;
		DataTransfer& t = sbc->transfer;
		t.SwitchToUsb(&device, 0);
		while (device.position < device.length)
		{
			const size_t before = device.position;
			if (t.DoTransfer() != TransferState::finished)
			{
				break;								// TaskLoop resets the connection
			}
			PoisonStale(t.usbRxHeader.dataLength);
			sbc->ProcessPackets();
			PoisonStale(SbcTransferBufferSize);
			RunMainTask();
			t.StartNextTransfer();
			if (device.position == before) { break; }
		}
		t.transportType = SbcTransportType::spi;
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	StartInput(data, size);
	if (setjmp(endOfInput) == 0)
	{
		if (size > 0 && (data[0] & 1) != 0)
		{
			RunUsb(data, size);
		}
		else
		{
			RunSpi(data, size);
		}
	}
	PoisonStale(SbcTransferBufferSize);
	return 0;
}


// ---------------------------------------------------------------------------------------------------------------------
// Seed inputs: well-formed packets of every request, as DSF writes them (DuetControlServer/Link/Protocol/Writer.cs)

constexpr size_t FuzzMaxInputSize = 3 + 3 * (3 + SbcTransferBufferSize);

namespace
{
	typedef std::vector<uint8_t> Bytes;

	template<class T> Bytes Raw(const T& v) { Bytes b(sizeof(T)); memcpy(b.data(), &v, sizeof(T)); return b; }
	Bytes Str(const char *s) { return Bytes(s, s + strlen(s)); }
	Bytes operator+(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

	struct Transfer
	{
		Bytes data;
		uint8_t numPackets = 0;
		void Packet(SbcRequest request, const Bytes& payload, uint16_t tag = 0)
		{
			PacketHeader h;
			h.request = (uint16_t)request;
			h.id = numPackets;
			h.length = (uint16_t)payload.size();
			h.resendPacketId = tag;
			data = data + Raw(h) + payload;
			while (data.size() % 4 != 0) { data.push_back(0); }
			++numPackets;
		}
	};

	// config: bit 0 = USB, bits 1-3 = waiting file operation, bit 7 = async write pending; gbFlags; read buffer size index
	Bytes Input(uint8_t config, uint8_t gbs, uint8_t bufferSize, const std::vector<Transfer>& transfers)
	{
		Bytes in = { config, gbs, bufferSize };
		for (const Transfer& t : transfers)
		{
			if ((config & 1) != 0)
			{
				UsbTransferHeader h;
				memset(&h, 0, sizeof(h));
				h.numPackets = t.numPackets;
				h.dataLength = (uint16_t)t.data.size();
				in = in + Raw(h) + t.data;
			}
			else
			{
				in = in + Bytes{ t.numPackets, (uint8_t)t.data.size(), (uint8_t)(t.data.size() >> 8) } + t.data;
			}
		}
		return in;
	}

	Bytes Code(uint8_t channel, char letter, int32_t major, const std::vector<std::pair<CodeParameter, Bytes>>& params)
	{
		CodeHeader h;
		memset(&h, 0, sizeof(h));
		h.channel = channel;
		h.flags = CodeFlags::HasMajorCommandNumber;
		h.numParameters = (uint8_t)params.size();
		h.letter = letter;
		h.majorCode = major;
		h.filePosition = 0xFFFFFFFF;
		Bytes b = Raw(h);
		for (const auto& p : params) { b = b + Raw(p.first); }
		for (const auto& p : params) { b = b + p.second; while (b.size() % 4 != 0) { b.push_back(0); } }
		return b;
	}
	std::pair<CodeParameter, Bytes> FloatParam(char letter, float f) { CodeParameter p; memset(&p, 0, sizeof(p)); p.letter = letter; p.type = DataType::Float; p.floatValue = f; return { p, Bytes() }; }
	std::pair<CodeParameter, Bytes> StringParam(char letter, const char *s, DataType t = DataType::String)
	{
		CodeParameter p; memset(&p, 0, sizeof(p)); p.letter = letter; p.type = t; p.intValue = (int32_t)strlen(s); return { p, Str(s) };
	}

	template<class T> Bytes Header(T h) { return Raw(h); }
}

// The files whose first frame in a sanitizer report is its key (fuzz_driver.inc)
static const char *const FuzzKeyFiles[] = { "DataTransfer.cpp", "SbcInterface.cpp", "OutputMemory.cpp" };

// For an ASan report: where the bad address is relative to the buffers of the SBC interface
static void FuzzDescribeAddress(uintptr_t a) noexcept
{
	struct { const char *name; const void *start; size_t length; } buffers[] =
	{
		{ "rxBuffer", sbc->transfer.rxBuffer, SbcTransferBufferSize },
		{ "txBuffer", sbc->transfer.txBuffer, SbcTransferBufferSize },
		{ "codeBuffer", sbc->codeBuffer, SbcCodeBufferSize },
		{ "fileReadBuffer", sbc->fileReadBuffer, sbc->fileBufferLength },
	};
	for (const auto& b : buffers)
	{
		const intptr_t offset = (intptr_t)(a - reinterpret_cast<uintptr_t>(b.start));
		if (offset >= -65536 && offset < (intptr_t)b.length + 0x40000)
		{
			fprintf(stderr, "FUZZ: bad address is %s%+" PRIdPTR " (%s is %zu bytes, received data length %u)\n", b.name, offset, b.name, b.length,
					(unsigned int)((sbc->transfer.transportType == SbcTransportType::usb) ? sbc->transfer.usbRxHeader.dataLength : sbc->transfer.rxHeader.dataLength));
		}
	}
}

static std::vector<Bytes> FuzzSeeds()
{
	std::vector<Bytes> seeds;
	auto one = [&seeds](SbcRequest r, const Bytes& payload, uint8_t config = 0, uint8_t gbs = 0, uint8_t bufferSize = 6, uint16_t tag = 0)
	{
		Transfer t;
		t.Packet(r, payload, tag);
		seeds.push_back(Input(config, gbs, bufferSize, { t }));
		seeds.push_back(Input(config | 1, gbs, bufferSize, { t }));
	};

	CodeChannelHeader cch; memset(&cch, 0, sizeof(cch)); cch.channel = GCodeChannel::SBC;
	one(SbcRequest::EmergencyStop, Bytes());
	one(SbcRequest::Code, Code(GCodeChannel::SBC, 'G', 1, { FloatParam('X', 10.0f), FloatParam('Y', 20.0f) }));
	one(SbcRequest::Code, Code(GCodeChannel::File, 'M', 117, { StringParam('@', "hello world") }), 0, 0, 6, ChannelEpochTagged | ChannelEpochFirstMarker | 1);
	one(SbcRequest::Code, Code(GCodeChannel::HTTP, 'G', 1, { StringParam('X', "{move.axes[0].max}", DataType::Expression) }), 0, gbWaitingForMacro);
	{
		GetObjectModelHeader h; h.keyLength = 4; h.flagsLength = 5;
		one(SbcRequest::GetObjectModel, Header(h) + Str("move") + Str("d99vn"));
	}
	{
		PrintStartedHeader h; memset(&h, 0, sizeof(h));
		h.filenameLength = 13; h.generatedByLength = 6; h.numFilaments = 2; h.fileSize = 12345; h.numLayers = 100;
		h.layerHeight = 0.2f; h.objectHeight = 20.0f; h.printTime = 3600; h.simulatedTime = 3500;
		const float filaments[2] = { 1000.0f, 2000.0f };
		one(SbcRequest::SetPrintFileInfo, Header(h) + Raw(filaments) + Str("0:/gcodes/a.g") + Str("Cura 5"));
	}
	{
		PrintStoppedHeader h; memset(&h, 0, sizeof(h)); h.reason = PrintStoppedReason::abort;
		one(SbcRequest::PrintStopped, Header(h));
		h.reason = PrintStoppedReason::normalCompletion;
		one(SbcRequest::PrintStopped, Header(h));
	}
	{
		MacroCompleteHeader h; memset(&h, 0, sizeof(h)); h.channel = GCodeChannel::Trigger; h.error = true;
		one(SbcRequest::MacroCompleted, Header(h));
		one(SbcRequest::MacroCompleted, Header(h), 0, gbWaitingForMacro);
	}
	one(SbcRequest::LockMovementAndWaitForStandstill, Header(cch), 0, 0, 6, ChannelEpochTagged);
	one(SbcRequest::Unlock, Header(cch));
	one(SbcRequest::WriteIap, Bytes(64, 0x5A));
	one(SbcRequest::AssignFilament_deprecated, Bytes(12, 0));
	one(SbcRequest::FileChunk_deprecated, Bytes(12, 0));
	one(SbcRequest::EvaluateExpression, Header(cch) + Str("move.axes[0].userPosition"));
	one(SbcRequest::EvaluateExpression, Header(cch) + Str("\"a string\""));
	one(SbcRequest::EvaluateExpression, Header(cch) + Str("Error"));
	one(SbcRequest::EvaluateExpression, Header(cch) + Str("f"));
	one(SbcRequest::EvaluateExpression, Header(cch) + Str("o"));
	{
		MessageHeader h; memset(&h, 0, sizeof(h)); h.messageType = (MessageType)(HttpMessage | GenericMessage); h.length = 12;
		one(SbcRequest::Message, Header(h) + Str("Hello world\n"));
		h.messageType = UsbMessage; h.length = 600;
		one(SbcRequest::Message, Header(h) + Bytes(600, 'x'), 0, gbExecutingOnSbc);
	}
	one(SbcRequest::MacroStarted, Header(cch), 0, gbWaitingForMacro);
	one(SbcRequest::InvalidateChannel, Header(cch), 0, 0, 6, ChannelEpochTagged | 1);
	{
		SetVariableHeader h; memset(&h, 0, sizeof(h)); h.channel = GCodeChannel::SBC; h.createVariable = true; h.variableLength = 10; h.expressionLength = 1;
		one(SbcRequest::SetVariable, Header(h) + Str("global.foo") + Str("5"));
		h.createVariable = false; h.variableLength = 10; h.expressionLength = 3;
		one(SbcRequest::SetVariable, Header(h) + Str("var.ab[2]") + Str(" ") + Str("abc"));
		h.variableLength = 7;
		one(SbcRequest::SetVariable, Header(h) + Str("var.abc") + Str("abc"));
	}
	{
		DeleteLocalVariableHeader h; memset(&h, 0, sizeof(h)); h.channel = GCodeChannel::SBC; h.variableLength = 3;
		one(SbcRequest::DeleteLocalVariable, Header(h) + Str("abc"));
	}
	{
		BooleanHeader h; memset(&h, 0, sizeof(h)); h.value = true;
		one(SbcRequest::CheckFileExistsResult, Header(h), 4 << 1);
		one(SbcRequest::FileDeleteResult, Header(h));
		one(SbcRequest::FileWriteResult, Header(h), 3 << 1);
		one(SbcRequest::FileWriteResult, Header(h), (6 << 1) | 0x80);
		one(SbcRequest::FileSeekResult, Header(h), 7 << 1);
		one(SbcRequest::FileTruncateResult, Header(h));
	}
	{
		OpenFileResult h; h.handle = 5; h.fileSize = 1000;
		one(SbcRequest::OpenFileResult, Header(h), 5 << 1);
	}
	{
		FileDataHeader h; h.bytesRead = 100;
		one(SbcRequest::FileReadResult, Header(h) + Bytes(100, 'g'), 1 << 1, 0, 7);
		one(SbcRequest::FileReadResult, Header(h) + Bytes(100, 'g'), 1 << 1, 0, 4);
		h.bytesRead = -1;
		one(SbcRequest::FileReadResult, Header(h), 1 << 1);
		one(SbcRequest::FileReadResult, Header(h));
	}
	{
		SetLastCodeResultHeader h; memset(&h, 0, sizeof(h)); h.channel = GCodeChannel::SBC; h.result = GCodeResult::warning;
		one(SbcRequest::SetLastCodeResult, Header(h));
	}
	one(SbcRequest::ObjectModelKeyChanged, Str("network"));
	one(SbcRequest::ObjectModelKeyChanged, Str("volumes"));
	{
		FileListEntry e; memset(&e, 0, sizeof(e)); e.size = 10; e.nameLength = 5;
		const Bytes entries = Raw(e) + Str("a.gco") + Bytes(3, 0);
		FileListHeader h; memset(&h, 0, sizeof(h)); h.dataLength = (uint32_t)entries.size(); h.endOfList = true;
		one(SbcRequest::FileListResult, Header(h) + entries, 2 << 1, 0, 6);
		one(SbcRequest::FileListResult, Header(h) + entries, 2 << 1, 0, 2);
	}
	one(SbcRequest::ActivateLinkCapabilities, Raw((uint32_t)7));

	// Several packets in one transfer, and two transfers
	{
		Transfer t1, t2;
		t1.Packet(SbcRequest::Code, Code(GCodeChannel::SBC, 'G', 0, { FloatParam('Z', 1.0f) }));
		t1.Packet(SbcRequest::EvaluateExpression, Header(cch) + Str("1+1"));
		t1.Packet(SbcRequest::Code, Code(GCodeChannel::SBC, 'M', 98, { StringParam('P', "foo.g") }));
		t2.Packet(SbcRequest::MacroCompleted, Raw(MacroCompleteHeader{ GCodeChannel::SBC, false, 0 }));
		t2.Packet(SbcRequest::Code, Code(GCodeChannel::File, 'G', 1, { FloatParam('E', 0.5f) }));
		t2.Packet(SbcRequest::InvalidateChannel, Raw(CodeChannelHeader{ GCodeChannel::File, 0, 0 }));
		seeds.push_back(Input(0, 0, 6, { t1, t2 }));
		seeds.push_back(Input(1, 0, 6, { t1, t2 }));
		seeds.push_back(Input(0, gbNoBuffer, 6, { t1, t2 }));
	}
	return seeds;
}

#ifdef FUZZ_LIBFUZZER
static void FuzzDeathCallback()
{
	if (__asan_report_present()) { FuzzDescribeAddress(reinterpret_cast<uintptr_t>(__asan_get_report_address())); }
}
extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
	__sanitizer_set_death_callback(FuzzDeathCallback);
	return 0;
}
#else
# include "fuzz_driver.inc"
#endif
