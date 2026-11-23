// Host test of a macro that DSF could not run, reported with SbcRequest::MacroCompleted and an error while the G-code buffer does not wait
// for the macro (e.g. a missing daemon.g). Compiled from the real text that the Makefile extracts:
// - SBC task: the MacroCompleted case of SbcInterface::ExchangeData and SbcInterface::RecordChannelAbort (src/SBC/SbcInterface.cpp)
// - main task: SbcInterface::ApplyRecordedAborts and SbcInterface::FillBuffer (src/SBC/SbcInterface.cpp), GCodeBuffer::Diagnostics, the
//   reader of M122 (src/GCodes/GCodeBuffer/GCodeBuffer.cpp)
// - the stack: GCodeBuffer::PopState and GCodeBuffer::CurrentFileMachineState (GCodeBuffer.cpp), GCodeMachineState::CloseFile and
//   GCodeMachineState::Pop (src/GCodes/GCodeMachineState.cpp)
//
// The main task (TaskPriority::SpinPriority) walks the stack of every G-code buffer for M122 without the buffer mutex, and the SBC task
// (TaskPriority::SbcPriority) has the higher priority, so it can run at any instruction of that walk. Whatever it deletes there, the walk
// may still hold. The test plays the SBC task in the middle of the walk, and checks what each task does to the stack. Deleted levels keep
// their memory until the end, so a level that is reached after its deletion is reported instead of being undefined behaviour.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from
#define _ecv_not_null(x) (x)

#define HAS_SBC_INTERFACE		1
#define HAS_MASS_STORAGE		0		// so that CloseFile takes its SBC path only
#define HAS_EMBEDDED_FILES		0
#define SUPPORT_ASYNC_MOVES		0
// TRACK_FILE_CODES stays undefined, as in src/SBC/SbcInterface.h

// The task that runs the code under test
enum class Task { main, sbc };
static Task currentTask = Task::main;
static const char *TaskName(Task t) noexcept { return (t == Task::main) ? "main" : "SBC"; }

static std::vector<std::string> failures;
static void Check(bool ok, const std::string& what) noexcept
{
	if (!ok)
	{
		failures.push_back(what);
	}
}

// What the code under test did to the stack and the records, in order
struct Event
{
	Task task;
	std::string what;
};
static std::vector<Event> events;
static void Log(const std::string& what) noexcept { events.push_back(Event{currentTask, what}); }

static unsigned int Count(Task t, const std::string& what) noexcept
{
	unsigned int n = 0;
	for (const Event& e : events)
	{
		if (e.task == t && e.what == what)
		{
			++n;
		}
	}
	return n;
}

static std::string EventsText() noexcept
{
	std::string s;
	for (const Event& e : events)
	{
		s += std::string((s.empty()) ? "" : ", ") + TaskName(e.task) + ": " + e.what;
	}
	return (s.empty()) ? "none" : s;
}

// RTOS. The SBC task gives up on a GB mutex after SbcYieldTimeout while the main task holds it, e.g. while it spins that GB
constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFF;
constexpr uint32_t SbcYieldTimeout = 10;

struct Mutex
{
	bool busyInMainTask = false;
	unsigned int holdCount = 0;
	Task holder = Task::main;
};

class MutexLocker
{
public:
	MutexLocker(Mutex& m, uint32_t = TimeoutUnlimited) noexcept : mutex(m), acquired(!(m.busyInMainTask && currentTask == Task::sbc))
	{
		if (acquired)
		{
			++mutex.holdCount;
			mutex.holder = currentTask;
		}
	}
	~MutexLocker() noexcept { Release(); }
	bool IsAcquired() const noexcept { return acquired; }
	void Release() noexcept
	{
		if (acquired)
		{
			--mutex.holdCount;
			acquired = false;
		}
	}

private:
	Mutex& mutex;
	bool acquired;
};

struct TaskCriticalSectionLocker
{
	TaskCriticalSectionLocker() noexcept { }
	~TaskCriticalSectionLocker() noexcept { }
};

static uint32_t millis() noexcept { return 1000; }

enum class Module { SbcInterface };
static void debugPrintf(const char *, ...) noexcept { }
#define REPORT_INTERNAL_ERROR		Check(false, "internal error reported at line " + std::to_string(__LINE__))
#define RRF_ASSERT(_expr)			Check(_expr, "RRF_ASSERT(" #_expr ") failed")

// Channels: the names and numbers of GCodeChannel.h that the test uses
class GCodeChannel
{
public:
	typedef uint8_t RawType;
	static constexpr RawType HTTP = 0, File = 2, Daemon = 9, NumValues = 15;

	GCodeChannel(size_t v) noexcept : value((RawType)v) { }
	bool IsValid() const noexcept { return value < NumValues; }
	RawType ToBaseType() const noexcept { return value; }
	RawType RawValue() const noexcept { return value; }
	const char *ToString() const noexcept { return (value == HTTP) ? "HTTP" : (value == Daemon) ? "Daemon" : "other"; }

private:
	RawType value;
};

constexpr size_t NumGCodeChannels = GCodeChannel::NumValues;

class StringRef
{
public:
	explicit StringRef(std::string& s) noexcept : str(s) { }
	void cat(const char *s) const noexcept { str += s; }
	void cat(char c) const noexcept { str += c; }
	void lcat(const char *s) const noexcept
	{
		if (!str.empty())
		{
			str += '\n';
		}
		str += s;
	}
	void catf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		char buf[100];
		va_list vargs;
		va_start(vargs, fmt);
		vsnprintf(buf, sizeof(buf), fmt, vargs);
		va_end(vargs);
		str += buf;
	}

private:
	std::string& str;
};

class GCodeBuffer;
static GCodeBuffer *_ecv_null gcodeSources[NumGCodeChannels];

class GCodes
{
public:
	GCodeBuffer *_ecv_null GetGCodeBuffer(GCodeChannel channel) const noexcept { return gcodeSources[channel.ToBaseType()]; }

	// As in GCodes.cpp: sets ChannelAbortRequest, which GCodes::Spin carries out at its start with ApplyRecordedAborts
	void ChannelAbortRecorded() noexcept
	{
		Log("request the apply");
		channelAbortRequested = true;
	}

	bool channelAbortRequested = false;
};

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return gcodes; }
	bool Debug(Module) const noexcept { return false; }
	bool UsingSbcInterface() const noexcept { return true; }
	void InputsUpdated() noexcept { }

private:
	GCodes gcodes;
};

static RepRap reprap;

// The stack of a GB. A deleted level keeps its memory until the end of the test and is remembered, so that every use of it is reported
typedef uint32_t FileId;
constexpr FileId NoFileId = 0;
enum class GCodeState : uint8_t { normal = 0 };
enum class GCodeResult : uint8_t { ok };

class GCodeException
{
public:
	bool IsNull() const noexcept { return true; }
};

class AutoStringHandle
{
public:
	void Assign(const char *_ecv_array _ecv_null s) noexcept
	{
		if (name != nullptr && s == nullptr)
		{
			Log(std::string("release the name ") + name);
		}
		name = s;
	}

private:
	const char *_ecv_array _ecv_null name = nullptr;
};

static std::set<const void *> deletedLevels;
static std::vector<void *> memoryToFree;

// The SBC task as it preempts the main task the next time the main task reads the state of a level
static void (*preemption)() noexcept = nullptr;

class GCodeMachineState
{
public:
	GCodeMachineState(const char *lbl, GCodeMachineState *_ecv_null prev) noexcept : previous(prev), label(lbl) { }
	~GCodeMachineState() noexcept { Log(std::string("delete the level ") + label); }

	static void operator delete(void *p) noexcept
	{
		deletedLevels.insert(p);
		memoryToFree.push_back(p);
	}

	GCodeMachineState *_ecv_null GetPrevious() const noexcept
	{
		if (deletedLevels.count(this) != 0)
		{
			Check(false, std::string("the ") + TaskName(currentTask) + " task follows the link of a level that has been deleted");
			return nullptr;
		}
		return previous;
	}

	GCodeState GetState() const noexcept
	{
		if (deletedLevels.count(this) != 0)
		{
			Check(false, std::string("the ") + TaskName(currentTask) + " task reads the state of a level that has been deleted");
			return GCodeState::normal;
		}
		const GCodeState s = state;
		if (preemption != nullptr && currentTask == Task::main)
		{
			void (*const p)() noexcept = preemption;
			preemption = nullptr;
			currentTask = Task::sbc;
			p();
			currentTask = Task::main;
		}
		return s;
	}

	// Real text from GCodeMachineState.cpp
	GCodeMachineState *Pop() const noexcept;
	void CloseFile() noexcept;

	GCodeMachineState *_ecv_null previous;
	FileId fileId = NoFileId;
	AutoStringHandle fname;
	GCodeException errorMessage;
	GCodeResult stateMachineResult = GCodeResult::ok;
	GCodeState state = GCodeState::normal;
	bool localPush = false;
	bool fileFinished = false;
	bool messageAcknowledged = false;
	bool waitingForAcknowledgement = false;
	bool lastCodeFromSbc = false;
	bool doingFileMacro = false;
	const char *label;
};

// A level that no code may see on the stack any more, or nullptr
static const GCodeMachineState *_ecv_null mustBeGone = nullptr;

static bool OnStack(const GCodeMachineState *_ecv_null top, const GCodeMachineState *_ecv_null level) noexcept
{
	for (const GCodeMachineState *_ecv_null ms = top; ms != nullptr && deletedLevels.count(ms) == 0; ms = ms->previous)
	{
		if (ms == level)
		{
			return true;
		}
	}
	return false;
}

enum class GCodeBufferState : uint8_t { parseNotStarted, ready, executing, executingOnSbc };

class StringParser
{
public:
	void ResetIndentationAfterPop() noexcept { }
};

#define IF_NOT_BINARY(_x)		{ if (!isBinaryBuffer) { _x; } }				// as in GCodeBuffer.cpp

class GCodeBuffer
{
public:
	explicit GCodeBuffer(GCodeChannel::RawType c) noexcept : machineState(new GCodeMachineState("base", nullptr)), codeChannel(c) { }

	// Real text from GCodeBuffer.cpp
	bool PopState(bool withinSameFile) noexcept;
	GCodeMachineState& CurrentFileMachineState() const noexcept;
	void Diagnostics(const StringRef& reply) noexcept;

	// What the real text calls, as in GCodeBuffer.h where it matters
	void Init() noexcept { Log("Init"); }
	void RestoreInvokingCommand(const GCodeMachineState&) noexcept { }
	void AppendFullCommand(const StringRef&) const noexcept { }
	bool IsBinary() const noexcept { return isBinaryBuffer; }
	bool IsDoingFileMacro(bool unlessClosed = false) const noexcept
	{
		return machineState->doingFileMacro || IsMacroRequestPending() || (macroFileClosed && !unlessClosed);
	}
	GCodeMachineState& LatestMachineState() const noexcept { return *machineState; }
	GCodeChannel GetChannel() const noexcept { return codeChannel; }
	bool IsFileChannel() const noexcept { return codeChannel.ToBaseType() == GCodeChannel::File; }
	bool IsCompletelyIdle() const noexcept { return true; }

	bool IsWaitingForMacro() const noexcept { return isWaitingForMacro; }
	bool IsMacroRequestPending() const noexcept { return macroRequestPending; }
	void MacroRequestSent() noexcept { macroRequestPending = false; }
	void ResolveMacroRequest(bool hadError, bool) noexcept
	{
		Log((hadError) ? "end the macro wait with an error" : "end the macro wait");
		isWaitingForMacro = false;
	}
	bool IsMacroFileClosed() const noexcept { return macroFileClosed; }
	void MacroFileClosedSent() noexcept { macroFileClosed = false; }
	bool IsMessageAcknowledged() const noexcept { return false; }
	bool IsAbortRequested() const noexcept { return false; }
	bool IsMessagePromptPending() const noexcept { return false; }
	bool IsInvalidated() const noexcept { return invalidated; }
	void Invalidate(bool i = true) noexcept { invalidated = i; }

	bool IsAbortRecorded() const noexcept { return abortRecorded; }
	void SetAbortRecorded(bool r) noexcept
	{
		if (r != abortRecorded)
		{
			Log((r) ? "record the abort" : "take the abort");
		}
		abortRecorded = r;
	}
	bool IsMacroErrorRecorded() const noexcept { return macroErrorRecorded; }
	void SetMacroErrorRecorded(bool r) noexcept
	{
		Check(mutex.holdCount != 0 && mutex.holder == currentTask,
				std::string("the ") + TaskName(currentTask) + " task changes the record of the failed macro without the GB mutex");
		Log((r) ? "record the failed macro" : "take the failed macro");
		macroErrorRecorded = r;
	}
	bool IsRecordPending() const noexcept { return abortRecorded || macroErrorRecorded; }
	bool IsConnectionEndRecorded() const noexcept { return connectionEndRecorded; }
	void SetConnectionEndRecorded() noexcept { connectionEndRecorded = true; }
	void ConnectionEndApplied() noexcept { connectionEndRecorded = false; }
	void EndConnectionOfCodes() noexcept { Log("EndConnectionOfCodes"); }
	void MarkCodeReplySurplus() noexcept { }

	void SetFileFinished() noexcept
	{
		Log("SetFileFinished");
		Check(!OnStack(machineState, mustBeGone), "SetFileFinished runs while the failed macro is still on the stack");
	}
	// The buffer of every stand-in GB exists from the start, so FillBuffer never takes its first-code path (GCodeBuffer.h)
	bool HasBuffer() const noexcept { return true; }
	void PrimeBinaryBuffer() noexcept { }
	void PutBinary(const uint32_t *, size_t, uint8_t) noexcept
	{
		Log("PutBinary");
		Check(!OnStack(machineState, mustBeGone), "a code of the SBC is put on the level of the failed macro");
	}
	void DecodeCommand() noexcept { }

	Mutex mutex;
	GCodeMachineState *machineState;
	StringParser stringParser;
	const GCodeChannel codeChannel;
	GCodeBufferState bufferState = GCodeBufferState::parseNotStarted;
	bool isBinaryBuffer = true;
	bool isWaitingForMacro = false;
	bool macroRequestPending = false;
	bool macroFileClosed = false;
	bool invalidated = false;
	bool abortRecorded = false;
	bool macroErrorRecorded = false;
	bool connectionEndRecorded = false;
};

// SBC interface: what the real text uses
enum class SbcRequest : uint16_t { MacroCompleted = 7, Code = 1 };

struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

constexpr uint16_t SbcCodeBufferSize = 4096;				// as in SbcMessageFormats.h

struct BufferedCodeHeader									// as in SbcMessageFormats.h
{
	bool isPending;
	uint8_t codeEpoch;
	uint16_t length;
};

struct CodeHeader											// FillBuffer reads only the channel
{
	uint8_t channel;
	uint8_t flags;
	uint8_t numParameters;
	char letter;
	int32_t majorCode;
};

struct DataTransfer
{
	GCodeChannel ReadMacroCompleteInfo(bool& error) noexcept
	{
		error = macroError;
		return macroChannel;
	}

	GCodeChannel macroChannel = GCodeChannel(GCodeChannel::Daemon);
	bool macroError = false;
};

alignas(4) static char codeBufferStorage[SbcCodeBufferSize];

class SbcInterface
{
public:
	bool HandleMacroCompleted(const PacketHeader *packet) noexcept;		// the case of ExchangeData, with its packetAcknowledged
	void StoreCode(GCodeChannel::RawType channel) noexcept;				// what the case of SbcRequest::Code stores in the ring

	// Real text from SbcInterface.cpp
	void RecordChannelAbort(GCodeBuffer& gb, bool connectionLost) noexcept;
	void ApplyRecordedAborts() noexcept;
	bool FillBuffer(GCodeBuffer &gb) noexcept;

	DataTransfer transfer;
	bool reportPause = false;
	uint32_t whenAbortRecorded = 0;
	uint32_t maxAbortApplyDelay = 0;
	char *codeBuffer = codeBufferStorage;
	volatile uint16_t rxPointer = 0, txPointer = 0, txEnd = 0;
	volatile bool sendBufferUpdate = false;

private:
	bool IsDroppedByEpoch(GCodeBuffer&, const PacketHeader&) noexcept { return false; }
	void InvalidateChannel(GCodeBuffer& gb) noexcept
	{
		Log("InvalidateChannel");
		Check(!OnStack(gb.machineState, mustBeGone), "the channel abort is applied while the failed macro is still on the stack");
	}
};

static SbcInterface sbc;

#include "GCodeMachineState.inc"
#include "GCodeBuffer.inc"
#include "SbcInterface.inc"

bool SbcInterface::HandleMacroCompleted(const PacketHeader *packet) noexcept
{
	bool packetAcknowledged = true;
	switch ((SbcRequest)packet->request)
	{
#include "MacroCompletedCase.inc"

	default:
		break;
	}
	return packetAcknowledged;
}

void SbcInterface::StoreCode(GCodeChannel::RawType channel) noexcept
{
	BufferedCodeHeader * const bufHeader = reinterpret_cast<BufferedCodeHeader *>(codeBuffer + txPointer);
	bufHeader->isPending = true;
	bufHeader->codeEpoch = 0;
	bufHeader->length = sizeof(CodeHeader);
	CodeHeader * const code = reinterpret_cast<CodeHeader *>(codeBuffer + txPointer + sizeof(BufferedCodeHeader));
	memset(code, 0, sizeof(CodeHeader));
	code->channel = channel;
	code->letter = 'G';
	txPointer += sizeof(BufferedCodeHeader) + sizeof(CodeHeader);
}

// The test scenario
static GCodeBuffer *daemonGb = nullptr;
static GCodeBuffer *httpGb = nullptr;
static GCodeMachineState *failedMacro = nullptr;

static void Discard(GCodeBuffer *_ecv_null gb) noexcept
{
	if (gb != nullptr)
	{
		for (GCodeMachineState *_ecv_null ms = gb->machineState; ms != nullptr && deletedLevels.count(ms) == 0; ms = ms->previous)
		{
			memoryToFree.push_back(ms);
		}
		delete gb;
	}
}

// The Daemon channel has started daemon.g, which it does not wait for (GCodeBuffer::RequestMacroFile): GCodes::DoFileMacro has pushed a level
// with the file. DSF answers with MacroCompleted and an error when the file cannot be opened. Without the push, the GB waits for the macro
static void Setup(bool pushed) noexcept
{
	Discard(daemonGb);
	Discard(httpGb);
	daemonGb = new GCodeBuffer(GCodeChannel::Daemon);
	httpGb = new GCodeBuffer(GCodeChannel::HTTP);
	for (GCodeBuffer *_ecv_null& gb : gcodeSources)
	{
		gb = nullptr;
	}
	gcodeSources[GCodeChannel::Daemon] = daemonGb;
	gcodeSources[GCodeChannel::HTTP] = httpGb;

	if (pushed)
	{
		failedMacro = new GCodeMachineState("daemon.g", daemonGb->machineState);
		failedMacro->fileId = 1;
		failedMacro->fname.Assign("daemon.g");
		failedMacro->doingFileMacro = true;
		daemonGb->machineState = failedMacro;
	}
	else
	{
		failedMacro = nullptr;
		daemonGb->isWaitingForMacro = true;
	}

	sbc.rxPointer = sbc.txPointer = sbc.txEnd = 0;
	reprap.GetGCodes().channelAbortRequested = false;
	currentTask = Task::main;
	preemption = nullptr;
	mustBeGone = nullptr;
	failures.clear();
	events.clear();
	deletedLevels.clear();
}

// The SBC task processes a MacroCompleted packet of the Daemon channel. Returns packetAcknowledged
static bool SbcSendsMacroCompleted(bool error) noexcept
{
	sbc.transfer.macroError = error;
	sbc.transfer.macroChannel = GCodeChannel(GCodeChannel::Daemon);
	const PacketHeader packet = { (uint16_t)SbcRequest::MacroCompleted, 0, 4, 0 };
	const Task before = currentTask;
	currentTask = Task::sbc;
	const bool acknowledged = sbc.HandleMacroCompleted(&packet);
	currentTask = before;
	return acknowledged;
}

// The start of GCodes::Spin: carry out ChannelAbortRequest
static void MainTaskSpinStarts() noexcept
{
	currentTask = Task::main;
	if (reprap.GetGCodes().channelAbortRequested)
	{
		reprap.GetGCodes().channelAbortRequested = false;
		sbc.ApplyRecordedAborts();
	}
}

static bool OnlyBase(const GCodeBuffer& gb) noexcept
{
	return deletedLevels.count(gb.machineState) == 0 && gb.machineState->previous == nullptr && strcmp(gb.machineState->label, "base") == 0;
}

static unsigned int failed = 0;
static unsigned int numCases = 0;

static void Report(const char *name) noexcept
{
	++numCases;
	if (failures.empty())
	{
		std::printf("PASS  %s\n", name);
		return;
	}
	++failed;
	std::printf("FAIL  %s\n", name);
	for (const std::string& f : failures)
	{
		std::printf("        %s\n", f.c_str());
	}
	std::printf("        events: %s\n", EventsText().c_str());
}

int main()
{
	// 1. M122 on any channel walks the stack of every GB in the main task (GCodes::Diagnostics -> GCodeBuffer::Diagnostics). The SBC task
	//    preempts it at the first level and processes the MacroCompleted packet with the error
	Setup(true);
	preemption = []() noexcept { (void)SbcSendsMacroCompleted(true); };
	{
		std::string text;
		daemonGb->Diagnostics(StringRef(text));
	}
	Check(preemption == nullptr, "the SBC task did not run during the walk");
	Report("M122 walks the stack while DSF reports the failed macro: the walk never reaches a deleted level");

	// 2. What the SBC task does
	Setup(true);
	{
		const bool acknowledged = SbcSendsMacroCompleted(true);
		Check(acknowledged, "the packet is requested again");
		Check(Count(Task::sbc, "delete the level daemon.g") == 0, "the SBC task deletes the level of the failed macro");
		Check(Count(Task::sbc, "release the name daemon.g") == 0, "the SBC task releases the file name of the failed macro");
		Check(Count(Task::sbc, "Init") == 0, "the SBC task initialises the parsers of the GB");
		Check(daemonGb->machineState == failedMacro, "the SBC task changes the stack");
		Check(daemonGb->IsMacroErrorRecorded(), "the failed macro is not recorded on the GB");
		Check(reprap.GetGCodes().channelAbortRequested, "the main task is not asked to apply the record");
	}
	Report("the SBC task records the failed macro, asks the main task to pop it and leaves the stack alone");

	// 3. What the main task does at the start of its next spin
	Setup(true);
	{
		(void)SbcSendsMacroCompleted(true);
		events.clear();
		MainTaskSpinStarts();
		const std::vector<std::string> expected = { "take the failed macro", "release the name daemon.g", "delete the level daemon.g", "Init" };
		bool asExpected = (events.size() == expected.size());
		for (size_t i = 0; asExpected && i < expected.size(); ++i)
		{
			asExpected = (events[i].task == Task::main && events[i].what == expected[i]);
		}
		Check(asExpected, "the main task does not take the record, close the file, delete the level once and then initialise the GB: " + EventsText());
		Check(OnlyBase(*daemonGb), "the base level is not on top of the stack of the Daemon GB");
		Check(OnlyBase(*httpGb), "the stack of another GB has changed");
		Check(!daemonGb->IsRecordPending(), "the record stays");
		events.clear();
		MainTaskSpinStarts();
		sbc.ApplyRecordedAborts();
		Check(events.empty(), "a further apply changes the GB again");
	}
	Report("the main task pops the failed macro once at the start of its next spin, with CloseFile, PopState and Init");

	// 4. DSF sends further packets of the channel in the same exchange. Those that act on the GB must see the stack after the pop
	Setup(true);
	{
		(void)SbcSendsMacroCompleted(true);
		mustBeGone = failedMacro;
		bool acknowledged = SbcSendsMacroCompleted(false);
		if (!acknowledged)
		{
			MainTaskSpinStarts();
			acknowledged = SbcSendsMacroCompleted(false);		// requested again
		}
		Check(acknowledged, "the next MacroCompleted is never accepted");
		Check(Count(Task::sbc, "SetFileFinished") == 1, "the next MacroCompleted is not processed exactly once");
	}
	Report("a further MacroCompleted of the channel acts on the stack only after the failed macro has been popped");

	// 5. The next code of the channel, which the SBC task stores after the record, while the main task still spins
	Setup(true);
	{
		(void)SbcSendsMacroCompleted(true);
		mustBeGone = failedMacro;
		currentTask = Task::sbc;
		sbc.StoreCode(GCodeChannel::Daemon);
		currentTask = Task::main;
		bool got = sbc.FillBuffer(*daemonGb);					// e.g. DoFilePrint, in the spin that the record came in
		if (!got)
		{
			MainTaskSpinStarts();
			got = sbc.FillBuffer(*daemonGb);
		}
		Check(got, "the code never reaches the GB");
		Check(Count(Task::main, "PutBinary") == 1, "the code does not reach the GB exactly once");
	}
	Report("a code that the SBC sends after the failed macro is put on the level below it");

	// 6. DSF aborts the channel (InvalidateChannel) after the failed macro, before the main task has applied the record
	Setup(true);
	{
		(void)SbcSendsMacroCompleted(true);
		currentTask = Task::sbc;
		sbc.RecordChannelAbort(*daemonGb, false);
		currentTask = Task::main;
		mustBeGone = failedMacro;
		events.clear();
		MainTaskSpinStarts();
		size_t pop = events.size(), abort = events.size();
		for (size_t i = 0; i < events.size(); ++i)
		{
			if (events[i].task == Task::main && events[i].what == "delete the level daemon.g") { pop = i; }
			if (events[i].task == Task::main && events[i].what == "InvalidateChannel") { abort = i; }
		}
		Check(pop < events.size(), "the main task does not pop the failed macro");
		Check(abort < events.size(), "the main task does not apply the abort");
		Check(pop < abort, "the abort is applied before the failed macro is popped");
		Check(!daemonGb->IsRecordPending(), "a record stays");
	}
	Report("an abort recorded after the failed macro is applied after the pop, in the same spin");

	// 7. Unchanged: while an abort is recorded, the SBC task keeps off the GB
	Setup(true);
	{
		currentTask = Task::sbc;
		sbc.RecordChannelAbort(*daemonGb, false);
		currentTask = Task::main;
		events.clear();
		const bool acknowledged = SbcSendsMacroCompleted(true);
		Check(!acknowledged, "the packet is not requested again");
		Check(daemonGb->machineState == failedMacro && !daemonGb->IsMacroErrorRecorded(), "the SBC task acts on the GB");
		Check(Count(Task::sbc, "delete the level daemon.g") == 0, "the SBC task deletes a level");
	}
	Report("a MacroCompleted while an abort of the channel is recorded is requested again");

	// 8. Unchanged: the main task holds the GB mutex
	Setup(true);
	{
		daemonGb->mutex.busyInMainTask = true;
		const bool acknowledged = SbcSendsMacroCompleted(true);
		daemonGb->mutex.busyInMainTask = false;
		Check(!acknowledged, "the packet is not requested again");
		Check(daemonGb->machineState == failedMacro, "the stack changes");
		Check(!daemonGb->IsMacroErrorRecorded() && !reprap.GetGCodes().channelAbortRequested, "the failed macro is recorded");
		Check(events.empty(), "something happens: " + EventsText());
	}
	Report("a MacroCompleted while the main task holds the GB mutex is requested again and changes nothing");

	// 9. Unchanged: a GB that waits for the macro gets the error as the end of its wait
	Setup(false);
	{
		const bool acknowledged = SbcSendsMacroCompleted(true);
		Check(acknowledged, "the packet is requested again");
		Check(Count(Task::sbc, "end the macro wait with an error") == 1, "the wait does not end with an error");
		Check(!daemonGb->IsRecordPending() && OnlyBase(*daemonGb), "the failed macro is recorded or the stack changes");
	}
	Report("a GB that waits for the macro gets the error as the end of the wait");

	// 10. Unchanged: a macro that DSF has run to its end is marked as finished in the SBC task, and the main task closes it in DoFilePrint
	Setup(true);
	{
		const bool acknowledged = SbcSendsMacroCompleted(false);
		Check(acknowledged, "the packet is requested again");
		Check(Count(Task::sbc, "SetFileFinished") == 1, "the macro is not marked as finished");
		Check(daemonGb->machineState == failedMacro && !daemonGb->IsRecordPending(), "the stack changes or a record is made");
	}
	Report("a MacroCompleted without an error marks the macro as finished and records nothing");

	Discard(daemonGb);
	Discard(httpGb);
	for (void *p : memoryToFree)
	{
		::operator delete(p);
	}
	std::printf("%u of %u failed\n", failed, numCases);
	return (failed == 0) ? 0 : 1;
}
