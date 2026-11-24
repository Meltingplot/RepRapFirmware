// Host test of the answers to SetVariable and EvaluateExpression requests from the SBC that do not fit into a transfer. Compiled from the real
// text that the Makefile extracts: the two cases of SbcInterface::ExchangeData and SbcInterface::MakeJsonAnswer (src/SBC/SbcInterface.cpp), the
// DataTransfer functions that write the answers into the transmit buffer (src/SBC/DataTransfer.cpp and .h), ObjectModel::ReportHeapArrayAsJson
// (src/ObjectModel/ObjectModel.cpp) and the OutputBuffer functions they use (src/Platform/OutputMemory.cpp). The output buffers are the real pool
// of 40 buffers of 256 bytes. See README.md.

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <General/String.h>
#include <General/StringRef.h>
#include <General/StringFunctions.h>
#include <General/SafeVsnprintf.h>
#include <General/SimpleMath.h>

// RRFLibraries' ecv headers define the annotations of the eCv verifier, except this one of RRF (src/RepRapFirmware.h)
#define THROWS(...)

// The boards with an SBC interface. The answers in this test have no 64-bit values
#define SUPPORT_BITMAP64		0
#define SUPPORT_CAN_EXPANSION	0
#define SUPPORTS_SBC_OVER_USB	0

static unsigned int internalErrors = 0;
#define REPORT_INTERNAL_ERROR	(++internalErrors)

typedef const char *c_string;

// As in src/Config/Configuration.h for SAME70 and SAME5x, the processors of the boards with an SBC interface
constexpr size_t OUTPUT_BUFFER_SIZE = 256;
constexpr size_t OUTPUT_BUFFER_COUNT = 40;
constexpr size_t RESERVED_OUTPUT_BUFFERS = 4;
constexpr size_t StringLength20 = 20;
constexpr size_t StringLength50 = 50;
constexpr size_t StringLength100 = 100;
constexpr size_t MaxVariableNameLength = 120;
constexpr size_t MaxGCodeStringLength = 256;
constexpr size_t MaxExpressionArrayIndices = 5;

// As in src/SBC/SbcMessageFormats.h
constexpr size_t SbcTransferBufferSize = 8192;
constexpr size_t MaxSbcExpressionLength = 256;

struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

enum class DataType : uint8_t
{
	Int = 0, UInt = 1, Float = 2, IntArray = 3, UIntArray = 4, FloatArray = 5, String = 6, Expression = 7, DriverId_dt = 8, DriverIdArray = 9,
	Boolean = 10, BoolArray = 11, ULong = 12, DateTime = 13, Null = 14, Char = 15, Bitmap16 = 16, Bitmap32 = 17, Bitmap64 = 18, FloatWithDigits = 19
};

struct EvaluationResultHeader
{
	DataType dataType;
	uint8_t channel;
	uint16_t expressionLength;
	union
	{
		int32_t intValue;
		uint32_t uintValue;
		float floatValue;
	};
};

enum class FirmwareRequest : uint16_t
{
	EvaluationResult = 11,
	VariableResult = 16,
};

enum class SbcRequest : uint16_t
{
	EvaluateExpression = 16,
	SetVariable = 20,
};

// The channel epoch is not part of this test: every request is untagged
static constexpr uint8_t ChannelEpochOf(uint16_t tag) noexcept { return 0; }
static constexpr uint16_t ChannelEpochTag(uint8_t epoch) noexcept { return 0; }

// As in src/SBC/SbcInterface.cpp
constexpr uint32_t SbcYieldTimeout = 10;

// The constants of the fix, extracted from src/SBC/SbcInterface.cpp
#include "SbcConstants.inc"

// Stand-ins for the RTOS and the platform
static uint32_t millis() noexcept { return 0; }
struct TaskCriticalSectionLocker { TaskCriticalSectionLocker() noexcept { } };		// the test runs in one thread

struct Mutex { };

class MutexLocker
{
public:
	MutexLocker(Mutex& m, uint32_t timeout) noexcept { }
	bool IsAcquired() const noexcept { return true; }
};

struct ReadWriteLock { };
struct ReadLocker
{
	explicit ReadLocker(ReadWriteLock&) noexcept { }
};

template<class T> class WriteLockedPointer
{
public:
	WriteLockedPointer(ReadWriteLock *lock, T *p) noexcept : ptr(p) { }
	T *operator->() const noexcept { return ptr; }

private:
	T *ptr;
};

struct Heap
{
	static ReadWriteLock heapLock;
};
ReadWriteLock Heap::heapLock;

enum class ErrorCode : uint32_t { OutputStarvation = 1 };

class GCodeBuffer;

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

class Platform
{
public:
	void LogError(ErrorCode e) noexcept { }
};

// A value as RRF keeps it, with the members that the extracted functions use. An array keeps its elements by reference, and vector() fills
// the new array with the same element value (ExpressionParser.cpp, Function::vector), so vector(128, vector(128, vector(30, 10))) holds
// 128 + 128 + 30 elements, not 491520
enum class TypeCode : uint8_t
{
	None, Bool_tc, Char, Uint32, Int32, Float, Uint64, Bitmap16, Bitmap32, Bitmap64, CString, HeapString, HeapArray, ObjectModelArray,
	DriverId_tc, IPAddress_tc, MacAddress_tc, DateTime_tc, Port_tc, UniqueId_tc, CanExpansionBoardDetails
};

struct ExpressionValue;
class ObjectModel;

class ArrayHandle
{
public:
	size_t GetNumElements() const noexcept { return (elements == nullptr) ? 0 : elements->size(); }
	void GetElement(size_t index, ExpressionValue& rslt) const noexcept;

	std::shared_ptr<std::vector<ExpressionValue>> elements;
};

class StringHandle
{
public:
	struct Locked
	{
		const char *Ptr() const noexcept { return p; }
		const char *p;
	};

	size_t GetLength() const noexcept { return s.size(); }
	Locked Get() const noexcept { return Locked{ s.c_str() }; }

	std::string s;
};

struct ExpressionValue
{
	TypeCode GetType() const noexcept { return type; }
	void AppendAsString(const StringRef& str) const noexcept { str.cat("192.168.1.10"); }
	uint64_t Get56BitValue() const noexcept { return 0; }

	TypeCode type = TypeCode::None;
	uint32_t param = 0;
	bool bVal = false;
	char cVal = 0;
	uint32_t uVal = 0;				// for an object model array of this test: the length of its JSON
	int32_t iVal = 0;
	float fVal = 0.0;
	const char *sVal = nullptr;
	StringHandle shVal;
	ArrayHandle ahVal;
	const ObjectModel *omVal = nullptr;
};

void ArrayHandle::GetElement(size_t index, ExpressionValue& rslt) const noexcept
{
	rslt = (*elements)[index];
}

static ExpressionValue Int(int32_t i) noexcept
{
	ExpressionValue v;
	v.type = TypeCode::Int32;
	v.iVal = i;
	return v;
}

static ExpressionValue Vector(size_t numElements, const ExpressionValue& element) noexcept
{
	ExpressionValue v;
	v.type = TypeCode::HeapArray;
	v.ahVal.elements = std::make_shared<std::vector<ExpressionValue>>(numElements, element);
	return v;
}

// As in src/ObjectModel/ObjectModel.h, with the members that the extracted functions use
class ObjectExplorationContext
{
public:
	ObjectExplorationContext() noexcept { }

	void AddIndex(int32_t index) noexcept { ++numIndices; }
	void RemoveIndex() noexcept { --numIndices; }
	unsigned int GetStartElement() const noexcept { return 0; }
	void SetNextElement(int arg) noexcept { nextElement = arg; }
	int GetNextElement() const noexcept { return nextElement; }
	size_t GetInitialBufferOffset() const noexcept { return 0; }

private:
	int nextElement = -1;
	unsigned int numIndices = 0;
};

struct ObjectModelClassDescriptor;

// Stand-in for the object model. ReportItemAsJson writes integers as ObjectModel::ReportItemAsJson does (catf with %" PRIi32 ") and arrays
// with the real ReportHeapArrayAsJson. ReportItemAsJsonFull stands for an object model array: it writes as many characters as the value says
class ObjectModel
{
public:
	void ReportHeapArrayAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelClassDescriptor *null classDescriptor,
								ArrayHandle ah, const char *_ecv_array filter) const THROWS(GCodeException);

	void ReportItemAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelClassDescriptor *null classDescriptor,
							const ExpressionValue& val, const char *_ecv_array filter) const
	{
		if (val.type == TypeCode::HeapArray)
		{
			ReportHeapArrayAsJson(buf, context, classDescriptor, val.ahVal, filter);
		}
		else
		{
			++integersReported;
			buf->catf("%" PRIi32, val.iVal);
		}
	}

	void ReportItemAsJsonFull(OutputBuffer *buf, ObjectExplorationContext& context, const ObjectModelClassDescriptor *_ecv_null classDescriptor,
								const ExpressionValue& val, const char *_ecv_array filter) const THROWS(GCodeException)
	{
		buf->cat('[');
		for (size_t i = 2; i < val.uVal; ++i)
		{
			buf->cat('1');
		}
		buf->cat(']');
	}

	static ReadWriteLock lifetimeLock;
	static size_t integersReported;
};

ReadWriteLock ObjectModel::lifetimeLock;
size_t ObjectModel::integersReported = 0;

static ObjectModel objectModelArrays;

static ExpressionValue ObjectModelArray(size_t jsonLength) noexcept
{
	ExpressionValue v;
	v.type = TypeCode::ObjectModelArray;
	v.omVal = &objectModelArrays;
	v.uVal = jsonLength;
	return v;
}

// As in src/ObjectModel/Variable.h, with the members that the extracted functions use. Assign and AssignIndexed copy the value, as the real ones do
class Variable
{
public:
	void Assign(ExpressionValue& ev) THROWS(GCodeException) { val = ev; }
	void AssignIndexed(const ExpressionValue& ev, size_t numIndices, const uint32_t *_ecv_array indices) THROWS(GCodeException)
	{
		if (val.type != TypeCode::HeapArray || numIndices != 1 || indices[0] >= val.ahVal.GetNumElements())
		{
			throw GCodeException("array index out of bounds");
		}
		std::vector<ExpressionValue> copy = *val.ahVal.elements;
		copy[indices[0]] = ev;
		val.ahVal.elements = std::make_shared<std::vector<ExpressionValue>>(copy);
	}

	ExpressionValue val;
};

class VariableSet
{
public:
	Variable *_ecv_null Lookup(const char *_ecv_array str, bool wantParameter) noexcept
	{
		const auto it = vars.find(str);
		return (it == vars.end()) ? nullptr : &it->second;
	}

	Variable *InsertNew(const char *_ecv_array str, ExpressionValue pVal, int16_t pScope) THROWS(GCodeException)
	{
		Variable& v = vars[str];
		v.val = pVal;
		return &v;
	}

	std::map<std::string, Variable> vars;
};

class GCodeBuffer
{
public:
	bool IsRecordPending() const noexcept { return false; }
	VariableSet& GetVariables() noexcept { return variables; }

	Mutex mutex;
	VariableSet variables;
};

class GCodeChannel
{
public:
	explicit GCodeChannel(uint8_t v) noexcept : value(v) { }
	bool IsValid() const noexcept { return value < 16; }
	uint8_t ToBaseType() const noexcept { return value; }

private:
	uint8_t value;
};

class GCodes
{
public:
	GCodeBuffer *GetGCodeBuffer(GCodeChannel channel) noexcept { return &gb; }

	GCodeBuffer gb;
};

// As in src/Platform/RepRap.h, with the members that the extracted functions use
class RepRap
{
public:
	Platform& GetPlatform() noexcept { return platform; }
	GCodes& GetGCodes() noexcept { return gcodes; }
	WriteLockedPointer<VariableSet> GetGlobalVariablesForWriting() noexcept { return WriteLockedPointer<VariableSet>(nullptr, &globals); }
	void GlobalUpdated() noexcept { ++globalSeq; }

	Platform platform;
	GCodes gcodes;
	VariableSet globals;
	unsigned int globalSeq = 0;
};

static RepRap reprap;

#include "OutputMemory.inc"
#include "ReportHeapArrayAsJson.inc"

// Stand-in for the expression parser. It knows integers, vector(n, value), var.name, global.name, a sum of two of them, and omarray(n), an
// object model array whose JSON is n characters long
class ExpressionParser
{
public:
	ExpressionParser(const GCodeBuffer *gbp, const char *_ecv_array text, const char *_ecv_array textEnd = nullptr) noexcept
		: gb(const_cast<GCodeBuffer*>(gbp)), p(text), end((textEnd != nullptr) ? textEnd : text + strlen(text)) { }

	ExpressionValue Parse() THROWS(GCodeException)
	{
		++evaluations;
		ExpressionValue v = ParseTerm();
		SkipSpaces();
		if (p < end && *p == '+')
		{
			++p;
			v.iVal += ParseTerm().iVal;
		}
		return v;
	}

	uint32_t ParseUnsigned() THROWS(GCodeException) { return ParseNumber(); }
	const char *_ecv_array GetEndptr() const noexcept { return p; }

	void CheckForExtraCharacters() THROWS(GCodeException)
	{
		SkipSpaces();
		if (p != end)
		{
			throw GCodeException("unexpected characters after expression");
		}
	}

	static unsigned int evaluations;

private:
	void SkipSpaces() noexcept { while (p < end && *p == ' ') { ++p; } }

	bool Skip(const char *s) noexcept
	{
		SkipSpaces();
		if (strncmp(p, s, strlen(s)) != 0)
		{
			return false;
		}
		p += strlen(s);
		return true;
	}

	uint32_t ParseNumber() THROWS(GCodeException)
	{
		SkipSpaces();
		if (p == end || *p < '0' || *p > '9')
		{
			throw GCodeException("expected a number");
		}
		uint32_t n = 0;
		while (p < end && *p >= '0' && *p <= '9')
		{
			n = n * 10 + (uint32_t)(*p++ - '0');
		}
		return n;
	}

	ExpressionValue ParseTerm() THROWS(GCodeException)
	{
		if (Skip("vector("))
		{
			const size_t n = ParseNumber();
			if (!Skip(","))
			{
				throw GCodeException("expected ','");
			}
			const ExpressionValue element = Parse();
			if (!Skip(")"))
			{
				throw GCodeException("expected ')'");
			}
			return Vector(n, element);
		}
		if (Skip("omarray("))
		{
			const size_t n = ParseNumber();
			if (!Skip(")"))
			{
				throw GCodeException("expected ')'");
			}
			return ObjectModelArray(n);
		}
		const bool isLocal = Skip("var."), isGlobal = !isLocal && Skip("global.");
		if (isLocal || isGlobal)
		{
			const char *start = p;
			while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
			{
				++p;
			}
			Variable *const v = ((isLocal) ? gb->GetVariables() : reprap.globals).Lookup(std::string(start, p).c_str(), false);
			if (v == nullptr)
			{
				throw GCodeException("unknown variable");
			}
			return v->val;
		}
		return Int((int32_t)ParseNumber());
	}

	GCodeBuffer *gb;
	const char *_ecv_array p;
	const char *_ecv_array end;
};

unsigned int ExpressionParser::evaluations = 0;

// As in src/SBC/DataTransfer.h, with the members that the extracted functions use. The read functions are stand-ins that return the request
// of the test
class DataTransfer
{
public:
	GCodeChannel ReadEvaluateExpression(size_t packetLength, const StringRef& expression) noexcept
	{
		expression.copy(requestText);
		return GCodeChannel(0);
	}

	GCodeChannel ReadSetVariable(bool& createVariable, const StringRef& varName, const StringRef& expression) noexcept
	{
		createVariable = requestCreate;
		varName.copy(requestName);
		expression.copy(requestText);
		return GCodeChannel(0);
	}

	bool WriteEvaluationResult(GCodeChannel channel, const char *expression, const ExpressionValue& value, uint16_t tag) noexcept;
	bool WriteEvaluationResult(GCodeChannel channel, const char *expression, OutputBuffer *json, uint16_t tag) noexcept;
	bool WriteEvaluationError(GCodeChannel channel, const char *expression, const char *errorMessage, uint16_t tag) noexcept;
	bool CanWriteSetVariableResult(const char *varName, const ExpressionValue& value) const noexcept;
	bool CanWriteSetVariableResult(const char *varName, const OutputBuffer *json) const noexcept;
	bool WriteSetVariableResult(GCodeChannel channel, const char *varName, const ExpressionValue& value, uint16_t tag) noexcept;
	bool WriteSetVariableResult(GCodeChannel channel,const char *varName, OutputBuffer *json, uint16_t tag) noexcept;
	bool WriteSetVariableError(GCodeChannel channel,const char *varName, const char *errorMessage, uint16_t tag) noexcept;

	// What the test sets up and reads back
	const char *requestName = "";
	const char *requestText = "";
	bool requestCreate = false;
	size_t txPointer = 0;
	uint16_t packetId = 0;
	struct { uint8_t numPackets = 1; } rxHeader;

private:
	static size_t GetSetVariableResultLength(const char *varName, const ExpressionValue& value, const StringRef& rslt) noexcept;
	uint8_t GetRxNumPackets() const noexcept;
	size_t FreeTxSpace() const noexcept;
	bool CanWritePacket(size_t dataLength = 0) const noexcept;
	PacketHeader *WritePacketHeader(FirmwareRequest request, size_t dataLength = 0, uint16_t resendPacketIdOrTag = 0) noexcept;
	void WriteData(const char *data, size_t length) noexcept;
	template<typename T> T *WriteDataHeader() noexcept;
	size_t AddPadding(size_t length) const noexcept;

public:
	alignas(8) char txBuffer[SbcTransferBufferSize];		// last, so that AddressSanitizer reports a write past it
};

#include "TransferInlines.inc"
#include "TransferFunctions.inc"

// As in src/SBC/SbcInterface.h, with the members that the extracted functions use. ProcessPacket is the part of ExchangeData that handles one
// packet and returns packetAcknowledged; false makes the SBC task ask DSF to send the request again (transfer.ResendPacket)
class SbcInterface : public ObjectModel
{
public:
	bool ProcessPacket(const PacketHeader *packet) noexcept;

	uint32_t burstModeStartTime = 0;

private:
	bool IsDroppedByEpoch(GCodeBuffer& gb, const PacketHeader& packet) noexcept { return false; }
	bool MakeJsonAnswer(OutputBuffer *_ecv_null &json, const ExpressionValue& val, size_t otherDataLength) THROWS(GCodeException);

public:
	DataTransfer transfer;			// last, see txBuffer
};

#include "SbcFunctions.inc"

bool SbcInterface::ProcessPacket(const PacketHeader *packet) noexcept
{
	bool packetAcknowledged = true;
	switch ((SbcRequest)packet->request)
	{
#include "SbcCases.inc"

	default:
		break;
	}
	return packetAcknowledged;
}

static SbcInterface sbc;

static unsigned int passed = 0, failed = 0;

// What the request wrote into the transmit buffer
struct Answer
{
	bool acknowledged = false;
	unsigned int packets = 0;
	FirmwareRequest request = FirmwareRequest::EvaluationResult;
	DataType dataType = DataType::Null;
	size_t dataLength = 0;					// of the packet
	std::string name;						// the variable name or the expression
	std::string text;						// a string, JSON or error message
	int32_t intValue = 0;
	std::string why;						// what went wrong in the transfer or the buffers
};

// Send one request in a transfer whose transmit buffer already holds txUsed bytes, and read back what the case wrote
static Answer Send(SbcRequest request, bool create, const char *name, const char *text, size_t txUsed = 0) noexcept
{
	DataTransfer& t = sbc.transfer;
	t.requestName = name;
	t.requestText = text;
	t.requestCreate = create;
	t.txPointer = txUsed;
	const uint16_t firstPacketId = t.packetId;
	const unsigned int freeBefore = OutputBuffer::GetFreeBuffers();
	internalErrors = 0;

	PacketHeader packet = {};
	packet.request = (uint16_t)request;
	Answer a;
	a.acknowledged = sbc.ProcessPacket(&packet);
	a.packets = t.packetId - firstPacketId;
	if (t.txPointer > SbcTransferBufferSize)
	{
		a.why += " wrote " + std::to_string(t.txPointer) + " bytes into a transfer of " + std::to_string(SbcTransferBufferSize);
	}
	if (OutputBuffer::GetFreeBuffers() != freeBefore)
	{
		a.why += " " + std::to_string(OutputBuffer::GetFreeBuffers()) + " free buffers after the request instead of " + std::to_string(freeBefore);
	}
	if (internalErrors != 0)
	{
		a.why += " internal error";
	}
	if (a.packets == 1)
	{
		const size_t start = (txUsed + 3) & ~(size_t)3;
		const PacketHeader *ph = reinterpret_cast<const PacketHeader *>(t.txBuffer + start);
		const EvaluationResultHeader *eh = reinterpret_cast<const EvaluationResultHeader *>(ph + 1);
		const char *data = reinterpret_cast<const char *>(eh + 1);
		a.request = (FirmwareRequest)ph->request;
		a.dataLength = ph->length;
		a.dataType = eh->dataType;
		a.intValue = eh->intValue;
		a.name.assign(data, eh->expressionLength);
		if (eh->dataType == DataType::Expression || eh->dataType == DataType::String)
		{
			a.text.assign(data + eh->expressionLength, eh->intValue);
		}
	}
	return a;
}

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

// Expected answers
static const char *const TooLong = "value too long to send to the SBC";

static std::string ExpectWaits(const Answer& a) noexcept
{
	std::string why = a.why;
	if (a.acknowledged)
	{
		why += " acknowledged";
	}
	if (a.packets != 0)
	{
		why += " wrote " + std::to_string(a.packets) + " packets";
	}
	return why;
}

static std::string ExpectAnswer(const Answer& a, FirmwareRequest request, DataType dataType, const char *name, const char *textStart, size_t textLength = 0) noexcept
{
	std::string why = a.why;
	if (!a.acknowledged)
	{
		why += " not acknowledged, so DSF sends the request again";
	}
	if (a.packets != 1)
	{
		why += " wrote " + std::to_string(a.packets) + " packets";
		return why;
	}
	if (a.request != request || a.dataType != dataType)
	{
		why += " answer has request " + std::to_string((int)a.request) + " and data type " + std::to_string((int)a.dataType) + ": " + a.text.substr(0, 80);
	}
	if (a.name != name)
	{
		why += " answer is for " + a.name;
	}
	if (textStart != nullptr && a.text.compare(0, strlen(textStart), textStart) != 0)
	{
		why += " answer is " + a.text.substr(0, 80);
	}
	if (textLength != 0 && a.text.size() != textLength)
	{
		why += " answer has " + std::to_string(a.text.size()) + " characters instead of " + std::to_string(textLength);
	}
	return why;
}

static std::string ExpectVariable(VariableSet& set, const char *name, bool exists, const char *value = nullptr) noexcept
{
	Variable *const v = set.Lookup(name, false);
	if ((v != nullptr) != exists)
	{
		return std::string(" variable ") + name + ((exists) ? " missing" : " exists");
	}
	if (v != nullptr && value != nullptr)
	{
		std::string actual;
		if (v->val.type == TypeCode::Int32)
		{
			actual = std::to_string(v->val.iVal);
		}
		else if (v->val.type == TypeCode::HeapArray)
		{
			actual = "array of " + std::to_string(v->val.ahVal.GetNumElements());
		}
		if (actual != value)
		{
			return std::string(" variable ") + name + " is " + actual + " instead of " + value;
		}
	}
	return "";
}

static std::string ExpectGlobalUpdates(unsigned int before, unsigned int expected) noexcept
{
	const unsigned int updates = reprap.globalSeq - before;
	return (updates == expected) ? "" : " " + std::to_string(updates) + " updates of the global variables instead of " + std::to_string(expected);
}

// Fill the transmit buffer so that this many bytes are free for the next packet, with one received packet (FreeTxSpace)
static size_t TxUsedForFreeSpace(size_t freeSpace) noexcept
{
	return SbcTransferBufferSize - sizeof(PacketHeader) - freeSpace;
}

int main()
{
	OutputBuffer::Init();
	VariableSet& globals = reprap.globals;
	VariableSet& locals = reprap.gcodes.gb.variables;
	constexpr FirmwareRequest VariableResult = FirmwareRequest::VariableResult, EvaluationResult = FirmwareRequest::EvaluationResult;

	// A small array is assigned and sent back as before
	{
		const unsigned int seq = reprap.globalSeq;
		const Answer a = Send(SbcRequest::SetVariable, true, "global.small", "vector(3, 10)");
		Report("global small = vector(3, 10) is assigned and answered",
				ExpectAnswer(a, VariableResult, DataType::Expression, "global.small", "[10,10,10]", 10) + ExpectVariable(globals, "small", true, "array of 3")
				+ ExpectGlobalUpdates(seq, 1));
	}

	// The globals of the lab printer (2026-11-24): vector(128, vector(128, vector(n, 10))). The answer is a root array, which
	// ReportHeapArrayAsJson cuts after the element that crosses half the buffer space, here after the first one. That element is
	// 128 * (3n + 1) + 129 bytes long: 7937 for n = 20, which fits, and 8321 for n = 21, which does not, so DSF sent the request again, and
	// the variable existed already then
	{
		const unsigned int seq = reprap.globalSeq;
		const Answer a = Send(SbcRequest::SetVariable, true, "global.m20", "vector(128, vector(128, vector(20, 10)))");
		Report("global m20 = vector(128, vector(128, vector(20, 10))) is assigned and answered with its first element",
				ExpectAnswer(a, VariableResult, DataType::Expression, "global.m20", "[[[10,10,10", 7939) + ExpectVariable(globals, "m20", true, "array of 128")
				+ ExpectGlobalUpdates(seq, 1));
	}
	for (unsigned int n : { 21, 30, 60 })
	{
		const unsigned int seq = reprap.globalSeq;
		const std::string name = "global.m" + std::to_string(n), text = "vector(128, vector(128, vector(" + std::to_string(n) + ", 10)))";
		const Answer a = Send(SbcRequest::SetVariable, true, name.c_str(), text.c_str());
		Report(("global m" + std::to_string(n) + " = " + text + " is refused").c_str(),
				ExpectAnswer(a, VariableResult, DataType::Expression, name.c_str(), TooLong, strlen(TooLong))
				+ ExpectVariable(globals, name.c_str() + strlen("global."), false) + ExpectGlobalUpdates(seq, 0));

		// The SBC does not send the request again, because the error was acknowledged. Had it, the variable would not exist yet either
	}

	// The same for local variables and for set
	{
		const Answer a = Send(SbcRequest::SetVariable, true, "var.m", "vector(128, vector(128, vector(30, 10)))");
		Report("var m = vector(128, vector(128, vector(30, 10))) is refused",
				ExpectAnswer(a, VariableResult, DataType::Expression, "var.m", TooLong) + ExpectVariable(locals, "m", false));
	}
	{
		const unsigned int seq = reprap.globalSeq;
		const Answer a = Send(SbcRequest::SetVariable, false, "global.small", "vector(128, vector(128, vector(30, 10)))");
		Report("set global.small = vector(128, vector(128, vector(30, 10))) is refused and keeps the old value",
				ExpectAnswer(a, VariableResult, DataType::Expression, "global.small", TooLong) + ExpectVariable(globals, "small", true, "array of 3")
				+ ExpectGlobalUpdates(seq, 0));
	}

	// The longest answer that a transfer can take: 8192 bytes, less the space for the resend request of the request (FreeTxSpace) and the packet
	// header, is 8176 bytes of data. vector(1, vector(3, vector(247, 1000000000))) is 3 * (11 * 247 + 2) + 3 = 8160 bytes of JSON, and the
	// EvaluationResultHeader and the name global.b make 16 more. One more character in the name does not fit
	{
		const Answer a = Send(SbcRequest::SetVariable, true, "global.b", "vector(1, vector(3, vector(247, 1000000000)))");
		Report("an answer of 8176 bytes, the longest that fits into a transfer, is sent",
				ExpectAnswer(a, VariableResult, DataType::Expression, "global.b", "[[[1000000000,", 8160) + ExpectVariable(globals, "b", true, "array of 1")
				+ ((a.dataLength == 8176) ? "" : " packet has " + std::to_string(a.dataLength) + " bytes of data instead of 8176"));
		const Answer b = Send(SbcRequest::SetVariable, true, "global.bb", "vector(1, vector(3, vector(247, 1000000000)))");
		Report("an answer of 8177 bytes is refused",
				ExpectAnswer(b, VariableResult, DataType::Expression, "global.bb", TooLong) + ExpectVariable(globals, "bb", false));
	}

	// Object model arrays are refused in the same way: one longer than a transfer, and one that overflows the output buffers
	{
		const Answer a = Send(SbcRequest::SetVariable, true, "global.om", "omarray(100)");
		Report("an object model array of 100 characters is assigned and answered",
				ExpectAnswer(a, VariableResult, DataType::Expression, "global.om", "[111", 100) + ExpectVariable(globals, "om", true));
		const Answer b = Send(SbcRequest::SetVariable, true, "global.om9", "omarray(9000)");
		Report("an object model array of 9000 characters is refused",
				ExpectAnswer(b, VariableResult, DataType::Expression, "global.om9", TooLong) + ExpectVariable(globals, "om9", false));
		const Answer c = Send(SbcRequest::SetVariable, true, "global.om20", "omarray(20000)");
		Report("an object model array of 20000 characters is refused",
				ExpectAnswer(c, VariableResult, DataType::Expression, "global.om20", TooLong) + ExpectVariable(globals, "om20", false));
	}

	// When the answer does not fit into the rest of the transfer, the request waits and the variable is not changed, so that it changes once
	// when the request comes again. Before, set var.i = var.i + 1 added 1 for each attempt, and global n = 5 failed the second time with
	// "variable already exists"
	{
		Send(SbcRequest::SetVariable, true, "var.i", "1");
		const Answer a = Send(SbcRequest::SetVariable, false, "var.i", "var.i + 1", TxUsedForFreeSpace(20));
		Report("set var.i = var.i + 1 waits while its answer does not fit", ExpectWaits(a) + ExpectVariable(locals, "i", true, "1"));
		const Answer b = Send(SbcRequest::SetVariable, false, "var.i", "var.i + 1");
		Report("set var.i = var.i + 1 sent again adds 1 once",
				ExpectAnswer(b, VariableResult, DataType::Int, "var.i", nullptr) + ExpectVariable(locals, "i", true, "2")
				+ ((b.intValue == 2) ? "" : " answer is " + std::to_string(b.intValue)));
	}
	{
		const unsigned int seq = reprap.globalSeq;
		const Answer a = Send(SbcRequest::SetVariable, true, "global.n", "5", TxUsedForFreeSpace(20));
		Report("global n = 5 waits while its answer does not fit", ExpectWaits(a) + ExpectVariable(globals, "n", false) + ExpectGlobalUpdates(seq, 0));
		const Answer b = Send(SbcRequest::SetVariable, true, "global.n", "5");
		Report("global n = 5 sent again creates the variable",
				ExpectAnswer(b, VariableResult, DataType::Int, "global.n", nullptr) + ExpectVariable(globals, "n", true, "5") + ExpectGlobalUpdates(seq, 1));
	}
	{
		const Answer a = Send(SbcRequest::SetVariable, true, "global.a", "vector(3, 10)", TxUsedForFreeSpace(28));
		Report("global a = vector(3, 10) waits while its answer does not fit", ExpectWaits(a) + ExpectVariable(globals, "a", false));
		const Answer b = Send(SbcRequest::SetVariable, true, "global.a", "vector(3, 10)");
		Report("global a = vector(3, 10) sent again creates the variable",
				ExpectAnswer(b, VariableResult, DataType::Expression, "global.a", "[10,10,10]") + ExpectVariable(globals, "a", true, "array of 3"));
	}

	// While other output holds the buffers, an array waits, so that one that overflows them is surely longer than a transfer
	{
		OutputBuffer *held[5] = {};
		for (OutputBuffer *&b : held) { OutputBuffer::Allocate(b); }
		const Answer a = Send(SbcRequest::SetVariable, true, "global.c", "vector(3, 10)");
		Report("with 35 free buffers global c = vector(3, 10) waits", ExpectWaits(a) + ExpectVariable(globals, "c", false));
		const Answer e = Send(SbcRequest::EvaluateExpression, false, "", "vector(3, 10)");
		Report("with 35 free buffers the expression vector(3, 10) waits", ExpectWaits(e));
		const Answer p = Send(SbcRequest::SetVariable, true, "global.d", "7");
		Report("with 35 free buffers global d = 7 is assigned", ExpectAnswer(p, VariableResult, DataType::Int, "global.d", nullptr) + ExpectVariable(globals, "d", true, "7"));
		OutputBuffer::ReleaseAll(held[4]);
		const Answer b = Send(SbcRequest::SetVariable, true, "global.c", "vector(3, 10)");
		Report("with 36 free buffers global c = vector(3, 10) is assigned",
				ExpectAnswer(b, VariableResult, DataType::Expression, "global.c", "[10,10,10]") + ExpectVariable(globals, "c", true, "array of 3"));
		for (OutputBuffer *&h : held) { OutputBuffer::ReleaseAll(h); }
	}

	// An error after the answer is made sends the error and releases the answer
	{
		Send(SbcRequest::SetVariable, true, "var.arr", "vector(2, 0)");
		const Answer a = Send(SbcRequest::SetVariable, false, "var.arr[5]", "vector(3, 10)");
		Report("set var.arr[5] = vector(3, 10) out of bounds sends the error",
				ExpectAnswer(a, VariableResult, DataType::Expression, "var.arr[5]", "array index out of bounds") + ExpectVariable(locals, "arr", true, "array of 2"));
	}

	// An expression whose value is too long to send gets the error. Before, the request waited for ever, and each attempt built the JSON again
	{
		const Answer a = Send(SbcRequest::EvaluateExpression, false, "", "vector(3, 10)");
		Report("the expression vector(3, 10) is answered", ExpectAnswer(a, EvaluationResult, DataType::Expression, "vector(3, 10)", "[10,10,10]", 10));
		const char *const longExpression = "vector(128, vector(128, vector(30, 10)))";
		const Answer b = Send(SbcRequest::EvaluateExpression, false, "", longExpression);
		Report("the expression vector(128, vector(128, vector(30, 10))) gets the error", ExpectAnswer(b, EvaluationResult, DataType::Expression, longExpression, TooLong));
		const Answer c = Send(SbcRequest::EvaluateExpression, false, "", "omarray(9000)");
		Report("the expression omarray(9000) gets the error", ExpectAnswer(c, EvaluationResult, DataType::Expression, "omarray(9000)", TooLong));
		const Answer d = Send(SbcRequest::EvaluateExpression, false, "", "omarray(100)", TxUsedForFreeSpace(28));
		Report("the expression omarray(100) waits while its answer does not fit", ExpectWaits(d));
		const Answer e = Send(SbcRequest::EvaluateExpression, false, "", "5");
		Report("the expression 5 is answered", ExpectAnswer(e, EvaluationResult, DataType::Int, "5", nullptr) + ((e.intValue == 5) ? "" : " answer is " + std::to_string(e.intValue)));
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
