// Host test of a GetObjectModel request from the SBC whose response does not fit into the output buffers. Compiled from the real text that
// the Makefile extracts: the case of SbcInterface::ExchangeData (src/SBC/SbcInterface.cpp), RepRap::GetModelResponse and StartJsonResponse
// (src/Platform/RepRap.cpp), ObjectModel::ReportHeapArrayAsJson (src/ObjectModel/ObjectModel.cpp) and the OutputBuffer functions they use
// (src/Platform/OutputMemory.cpp). The output buffers are the real pool of 40 buffers of 256 bytes. See README.md.

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <General/String.h>
#include <General/StringRef.h>
#include <General/SafeVsnprintf.h>
#include <General/SimpleMath.h>

// RRFLibraries' ecv headers define the annotations of the eCv verifier, except this one of RRF (src/RepRapFirmware.h)
#define THROWS(...)

typedef const char *c_string;

// As in src/Config/Configuration.h for SAME70 and SAME5x, the processors of the boards with an SBC interface
constexpr size_t OUTPUT_BUFFER_SIZE = 256;
constexpr size_t OUTPUT_BUFFER_COUNT = 40;
constexpr size_t RESERVED_OUTPUT_BUFFERS = 4;
constexpr size_t StringLength20 = 20;
constexpr size_t StringLength100 = 100;

// As in src/SBC/SbcMessageFormats.h
constexpr size_t SbcTransferBufferSize = 8192;

struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

struct StringHeader
{
	uint16_t length;
	uint16_t padding;
};

enum class SbcRequest : uint16_t
{
	GetObjectModel = 1,
};

// As in src/SBC/SbcInterface.cpp
constexpr uint32_t SbcYieldTimeout = 10;

// The real threshold of the fix, extracted from src/SBC/SbcInterface.cpp
#include "SbcConstants.inc"

// Stand-ins for the RTOS and the platform
static uint32_t millis() noexcept { return 0; }
struct TaskCriticalSectionLocker { TaskCriticalSectionLocker() noexcept { } };		// the test runs in one thread

struct Mutex
{
	bool heldByOtherTask = false;
};

class MutexLocker
{
public:
	MutexLocker(Mutex& m, uint32_t timeout) noexcept : acquired(!m.heldByOtherTask) { }
	bool IsAcquired() const noexcept { return acquired; }

private:
	bool acquired;
};

struct ReadWriteLock { };
struct ReadLocker
{
	explicit ReadLocker(ReadWriteLock&) noexcept { }
};

struct Heap
{
	static ReadWriteLock heapLock;
};
ReadWriteLock Heap::heapLock;

enum class ErrorCode : uint32_t { OutputStarvation = 1 };
enum MessageType : uint32_t { ErrorMessage = 1 };

class GCodeBuffer
{
public:
	bool HadExplicitLineNumber() const noexcept { return false; }
	long GetExplicitLineNumber() const noexcept { return 0; }
};

class GCodeException
{
public:
	explicit GCodeException(const char *msg) noexcept : message(msg) { }
	void GetMessage(const StringRef& reply, const GCodeBuffer *gb) const noexcept { reply.copy(message); }

private:
	const char *message;
};

// As in src/Platform/OutputMemory.h, with the members that the extracted functions use
class OutputBuffer
{
public:
	explicit OutputBuffer(OutputBuffer *_ecv_null n) noexcept : next(n) { }
	OutputBuffer(const OutputBuffer&) = delete;

	OutputBuffer *null Next() const noexcept { return next; }
	bool HadOverflow() const noexcept { return hadOverflow; }
	const char *_ecv_array Data() const noexcept { return data; }
	const char *_ecv_array UnreadData() const noexcept { return data + bytesRead; }
	size_t DataLength() const noexcept { return dataLength; }
	size_t Length() const noexcept;
	size_t BytesLeft() const noexcept { return dataLength - bytesRead; }
	void UpdateWhenQueued() noexcept;

	size_t vcatf(const char *_ecv_array fmt, va_list vargs) noexcept;
	size_t catf(const char *_ecv_array fmt, ...) noexcept __attribute__ ((format (printf, 2, 3)));

	size_t copy(const char c) noexcept;
	size_t copy(const char *_ecv_array src) noexcept;
	size_t copy(const char *_ecv_array src, size_t len) noexcept;

	size_t cat(const char c) noexcept;
	size_t cat(const char *_ecv_array src) noexcept;
	size_t cat(const char *_ecv_array src, size_t len) noexcept;
	size_t cat(StringRef &str) noexcept;

	static void Init() noexcept;
	static bool Allocate(OutputBuffer *_ecv_null &buf, bool allowReserved = true) noexcept;
	__attribute__((warn_unused_result)) static OutputBuffer *Release(OutputBuffer *buf) noexcept;
	static void ReleaseAll(OutputBuffer *_ecv_null volatile &buf) noexcept;
	static unsigned int GetFreeBuffers() noexcept { return OUTPUT_BUFFER_COUNT - usedOutputBuffers; }

private:
	void Clear() noexcept;

	OutputBuffer *_ecv_null next;
	OutputBuffer *last;
	uint32_t whenQueued;
	char data[OUTPUT_BUFFER_SIZE];
	size_t dataLength, bytesRead;
	bool isReferenced;
	bool hadOverflow;
	std::atomic<size_t> references;

	static OutputBuffer *_ecv_null volatile freeOutputBuffers;
	static std::atomic<size_t> usedOutputBuffers;
	static volatile size_t maxUsedOutputBuffers;
};

// As in src/Platform/OutputMemory.cpp
OutputBuffer *_ecv_null volatile OutputBuffer::freeOutputBuffers = nullptr;
std::atomic<size_t> OutputBuffer::usedOutputBuffers = 0;
volatile size_t OutputBuffer::maxUsedOutputBuffers = 0;

// Stand-in for the platform, which records the messages
class Platform
{
public:
	void LogError(ErrorCode e) noexcept { }
	void MessageF(MessageType type, const char *fmt, ...) noexcept __attribute__ ((format (printf, 3, 4)))
	{
		char text[300];
		va_list vargs;
		va_start(vargs, fmt);
		SafeVsnprintf(text, sizeof(text), fmt, vargs);
		va_end(vargs);
		messages.push_back(text);
	}

	std::vector<std::string> messages;
};

// A value as RRF keeps it. An array keeps its elements by reference, and vector() fills the new array with the same element value
// (ExpressionParser.cpp, Function::vector), so vector(128, vector(128, vector(30, 10))) holds 128 + 128 + 30 elements, not 491520
struct ExpressionValue;

class ArrayHandle
{
public:
	size_t GetNumElements() const noexcept { return (elements == nullptr) ? 0 : elements->size(); }
	void GetElement(size_t index, ExpressionValue& rslt) const noexcept;

	std::shared_ptr<std::vector<ExpressionValue>> elements;
};

struct ExpressionValue
{
	bool isArray = false;
	int32_t iVal = 0;
	ArrayHandle ahVal;
};

void ArrayHandle::GetElement(size_t index, ExpressionValue& rslt) const noexcept
{
	rslt = (*elements)[index];
}

static ExpressionValue Int(int32_t i) noexcept
{
	ExpressionValue v;
	v.iVal = i;
	return v;
}

static ExpressionValue Vector(size_t numElements, const ExpressionValue& element) noexcept
{
	ExpressionValue v;
	v.isArray = true;
	v.ahVal.elements = std::make_shared<std::vector<ExpressionValue>>(numElements, element);
	return v;
}

// As in src/ObjectModel/ObjectModel.h, with the members that ReportHeapArrayAsJson uses
class ObjectExplorationContext
{
public:
	explicit ObjectExplorationContext(size_t initialBufferOffset) noexcept : initialBufOffset(initialBufferOffset) { }

	void AddIndex(int32_t index) noexcept { ++numIndices; }
	void RemoveIndex() noexcept { --numIndices; }
	unsigned int GetStartElement() const noexcept { return 0; }
	void SetNextElement(int arg) noexcept { nextElement = arg; }
	int GetNextElement() const noexcept { return nextElement; }
	size_t GetInitialBufferOffset() const noexcept { return initialBufOffset; }

private:
	size_t initialBufOffset;
	int nextElement = -1;
	unsigned int numIndices = 0;
};

struct ObjectModelClassDescriptor;

// Stand-in for the object model. ReportItemAsJson writes integers as ObjectModel::ReportItemAsJson does (catf with %" PRIi32 ") and
// arrays with the real ReportHeapArrayAsJson. ReportAsJson reports the key "global" with one variable "m", as the real one reports the
// global variables: an object, so that the array in it is not the root array and is not cut after half the buffer space
class ObjectModel
{
public:
	void ReportHeapArrayAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelClassDescriptor *null classDescriptor,
								ArrayHandle ah, const char *_ecv_array filter) const THROWS(GCodeException);

	void ReportItemAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelClassDescriptor *null classDescriptor,
							const ExpressionValue& val, const char *_ecv_array filter) const
	{
		if (val.isArray)
		{
			ReportHeapArrayAsJson(buf, context, classDescriptor, val.ahVal, filter);
		}
		else
		{
			++integersReported;
			buf->catf("%" PRIi32, val.iVal);
		}
	}

	void ReportAsJson(const GCodeBuffer *_ecv_null gb, OutputBuffer *buf, const char *_ecv_array filter, const char *_ecv_array reportFlags, bool wantArrayLength) const
	{
		if (strcmp(filter, "global") != 0)
		{
			throw GCodeException("unknown key");
		}
		ObjectExplorationContext context(buf->Length());
		buf->cat("{\"m\":");
		ReportItemAsJson(buf, context, nullptr, globalM, "");
		buf->cat('}');
	}

	ExpressionValue globalM;
	mutable size_t integersReported = 0;
};

// As in src/Platform/RepRap.h, with the members that the extracted functions use
class RepRap : public ObjectModel
{
public:
	OutputBuffer *GetModelResponse(const GCodeBuffer *_ecv_null gb, c_string _ecv_null key, c_string _ecv_null flags) const THROWS(GCodeException);
	static void StartJsonResponse(const GCodeBuffer *_ecv_null gb, OutputBuffer *outbuf) noexcept;

	Platform& GetPlatform() noexcept { return platform; }
	Mutex& GetObjectModelReportMutex() noexcept { return objectModelReportMutex; }

	Platform platform;
	Mutex objectModelReportMutex;
	unsigned int modelResponses = 0;
};

static RepRap reprap;

#include "OutputMemory.inc"
#include "GetModelResponse.inc"
#include "ReportHeapArrayAsJson.inc"

// Stand-in for DataTransfer. WriteObjectModel writes like the real one (src/SBC/DataTransfer.cpp): it returns false and keeps the buffers
// when the packet does not fit, else it releases them
class DataTransfer
{
public:
	void ReadGetObjectModel(size_t packetLength, const StringRef& key, const StringRef& flags) noexcept
	{
		key.copy(requestedKey);
		flags.copy(requestedFlags);
	}

	bool WriteObjectModel(OutputBuffer *data) noexcept
	{
		const size_t dataLength = (data != nullptr) ? data->Length() : 0;
		if (sizeof(PacketHeader) + sizeof(StringHeader) + dataLength > txSpace)
		{
			return false;
		}
		++objectModelsWritten;
		response.clear();
		while (data != nullptr)
		{
			response.append(data->UnreadData(), data->BytesLeft());
			data = OutputBuffer::Release(data);
		}
		return true;
	}

	const char *requestedKey = "global";
	const char *requestedFlags = "d99v";
	size_t txSpace = SbcTransferBufferSize;
	unsigned int objectModelsWritten = 0;
	std::string response;
};

static DataTransfer transfer;

// The part of SbcInterface::ExchangeData that handles one packet. Returns packetAcknowledged; false makes the SBC task ask DSF to send the
// request again (transfer.ResendPacket)
static bool ProcessPacket(const PacketHeader *packet) noexcept
{
	bool packetAcknowledged = true;
	switch ((SbcRequest)packet->request)
	{
#include "GetObjectModelCase.inc"

	default:
		break;
	}
	return packetAcknowledged;
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
}

struct Expected
{
	bool acknowledged;
	bool written;								// an object model response was written
	const char *responseStart;					// what the response starts with, nullptr for an empty one
	const char *message;						// what the message starts with, nullptr for none
	size_t maxIntegersReported;
};

// Send one GetObjectModel request for the key "global" and check what the case did with it
static void Check(const char *name, const ExpressionValue& value, const Expected& e) noexcept
{
	reprap.globalM = value;
	reprap.integersReported = 0;
	reprap.platform.messages.clear();
	transfer.objectModelsWritten = 0;
	transfer.response.clear();
	const unsigned int freeBefore = OutputBuffer::GetFreeBuffers();

	PacketHeader packet = {};
	packet.request = (uint16_t)SbcRequest::GetObjectModel;
	const bool acknowledged = ProcessPacket(&packet);

	std::string why;
	if (acknowledged != e.acknowledged)
	{
		why += (acknowledged) ? " acknowledged" : " not acknowledged, so DSF sends the request again";
	}
	if ((transfer.objectModelsWritten != 0) != e.written)
	{
		why += (e.written) ? " no response written" : " response written";
	}
	else if (e.written)
	{
		if (e.responseStart == nullptr && !transfer.response.empty())
		{
			why += " response not empty: " + transfer.response.substr(0, 60);
		}
		else if (e.responseStart != nullptr && transfer.response.compare(0, strlen(e.responseStart), e.responseStart) != 0)
		{
			why += " response is " + transfer.response.substr(0, 80);
		}
	}
	if (e.message == nullptr && !reprap.platform.messages.empty())
	{
		why += " message \"" + reprap.platform.messages[0] + "\"";
	}
	else if (e.message != nullptr && (reprap.platform.messages.size() != 1 || reprap.platform.messages[0].compare(0, strlen(e.message), e.message) != 0))
	{
		why += (reprap.platform.messages.empty()) ? std::string(" no message") : " message \"" + reprap.platform.messages[0] + "\"";
	}
	if (reprap.integersReported > e.maxIntegersReported)
	{
		why += " reported " + std::to_string(reprap.integersReported) + " integers, more than " + std::to_string(e.maxIntegersReported);
	}
	if (OutputBuffer::GetFreeBuffers() != freeBefore)
	{
		why += " " + std::to_string(OutputBuffer::GetFreeBuffers()) + " free buffers after the request instead of " + std::to_string(freeBefore);
	}
	Report(name, why);
}

int main()
{
	OutputBuffer::Init();

	// A small global is reported as before
	Check("a small global is reported", Vector(3, Int(10)),
			{ true, true, "{\"key\":\"global\",\"flags\":\"d99v\",\"result\":{\"m\":[10,10,10]}}\n", nullptr, 3 });

	// The global of the lab printer (2026-11-24): vector(128, vector(128, vector(n, 10))). Its JSON is about 1 MB for n = 20, far more than
	// the 36 buffers of 256 bytes that a response may use. DSF sent the request again every 1.75 s (n = 20) to more than 4 s (n = 60), and
	// RRF built the whole response each time. The integers that fit into the buffers are about 9216 / 3, so allow 3100
	const Expected tooLong = { true, true, nullptr, "Cannot store excessively long object model response, discarding request (output buffers full, key global, flags d99v)", 3100 };
	Check("vector(128, vector(128, vector(20, 10))) is discarded", Vector(128, Vector(128, Vector(20, Int(10)))), tooLong);
	Check("vector(128, vector(128, vector(30, 10))) is discarded", Vector(128, Vector(128, Vector(30, Int(10)))), tooLong);
	Check("vector(128, vector(128, vector(60, 10))) is discarded", Vector(128, Vector(128, Vector(60, Int(10)))), tooLong);

	// A response that fits into the output buffers but not into a transfer was already discarded. 100 arrays of 42 integers make
	// 100 * 85 + 99 + 2 = 8601 bytes of JSON, and the response around them 48 more
	Check("a response longer than a transfer is discarded with its length", Vector(100, Vector(42, Int(1))),
			{ true, true, nullptr, "Cannot store excessively long object model response, discarding request (total length 8649, key global, flags d99v)", 4200 });

	// While other output holds the buffers, wait: the request is sent again, and nothing is built. 36 free buffers are needed, so that a
	// response that overflows them is surely longer than a transfer
	{
		OutputBuffer *held[5] = {};
		for (OutputBuffer *&b : held) { OutputBuffer::Allocate(b); }
		Check("with 35 free buffers the request waits", Vector(3, Int(10)), { false, false, nullptr, nullptr, 0 });
		OutputBuffer::ReleaseAll(held[4]);
		Check("with 36 free buffers a small global is reported", Vector(3, Int(10)),
				{ true, true, "{\"key\":\"global\",\"flags\":\"d99v\",\"result\":{\"m\":[10,10,10]}}\n", nullptr, 3 });
		Check("with 36 free buffers the large global is discarded", Vector(128, Vector(128, Vector(20, Int(10)))), tooLong);
		for (OutputBuffer *&b : held) { OutputBuffer::ReleaseAll(b); }
	}

	// While another task reports the object model, wait as well
	reprap.objectModelReportMutex.heldByOtherTask = true;
	Check("while another object model report runs the request waits", Vector(3, Int(10)), { false, false, nullptr, nullptr, 0 });
	reprap.objectModelReportMutex.heldByOtherTask = false;

	// When the transfer has no room left, the request is sent again as before
	transfer.txSpace = sizeof(PacketHeader) + sizeof(StringHeader) - 1;			// not even an empty response fits
	Check("a response that does not fit into the rest of the transfer waits", Vector(3, Int(10)), { false, false, nullptr, nullptr, 3 });
	Check("an empty response that does not fit into the rest of the transfer waits", Vector(128, Vector(128, Vector(20, Int(10)))),
			{ false, false, nullptr, nullptr, 3100 });
	transfer.txSpace = SbcTransferBufferSize;

	// An error is still sent back as the response
	transfer.requestedKey = "unknown";
	Check("an unknown key returns the error", Vector(3, Int(10)), { true, true, "unknown key", nullptr, 0 });
	transfer.requestedKey = "global";

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
