// Host test of the announcement of an expansion board that restarts while a remote accelerometer collection runs on it, in SBC mode.
//
// The CAN receiver task handles the announcement (ExpansionManager::ProcessAnnouncement in src/CAN/ExpansionManager.cpp). If the board
// restarted, the collection that ran on it is lost, and Accelerometers::RemoteBoardRestarted (src/Accelerometers/Accelerometers.cpp)
// finishes the accelerometer file: it writes a line, truncates the file and closes it. In SBC mode the file is on the SBC, and the
// truncate and the close each wait up to SbcMaxRequestTime for the SBC task (SbcInterface::DoFileOperation in src/SBC/SbcInterface.cpp).
// The SBC task also answers the object model requests of DSF. The live poll of DSF reports boards[].drivers, whose array table entry
// names ExpansionManager::boardsLock (objectModelArrayTable in ExpansionManager.cpp), so the SBC task holds that lock for reading while
// it reports the array (ObjectModel::ReportObjectModelArrayAsJson). If the CAN receiver finishes the file while it holds boardsLock for
// writing, and a poll is waiting, the SBC task waits for the lock, the file operation runs into its timeout, and the CAN receiver, which
// handles all CAN input, and the SBC task stand still until then.
//
// The test compiles the real text of ProcessAnnouncement and RemoteBoardRestarted and the real ReadWriteLock, ReadLocker and WriteLocker
// of RRFLibraries, which the Makefile extracts at build time. It runs the tasks as threads on one simulated processor: exactly one task
// runs at a time, and a task runs until it blocks unless a task with a higher priority becomes ready. A wait with a timeout expires when
// no task can run any more. The calls that the real code makes into stand-ins are the points where the other task's work arrives: a
// poll of DSF for the SBC task, or the announcement for the CAN receiver. The test tries every such point in turn.

#define RTOS							// compile the members that RRFLibraries declares only for an RTOS

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// eCv annotations used by the RRF and RRFLibraries sources
#define _ecv_array
#define _ecv_null
#define _ecv_from
#define null
#define _ecv_not_null(x)	(x)
#define not_null(x)			(x)

static std::vector<std::string> failures;

static void Fail(const std::string& s) noexcept
{
	failures.push_back(s);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// One processor. Each task is a thread, and only the task that 'running' names runs; the others wait in SwitchTo.

class TaskBase;

namespace Cpu
{
	static std::mutex mutex;
	static std::condition_variable cv;
	static TaskBase *running = nullptr;
	static std::vector<TaskBase *> tasks;
	static unsigned int suspended = 0;		// nesting of vTaskSuspendAll
	static bool yieldPending = false;		// a task with a higher priority became ready while the scheduler was suspended
	static bool allFinished = false;

	static void SwitchTo(TaskBase *next) noexcept;
	static void Reschedule() noexcept;
	static void MaybePreempt() noexcept;
	static void Block(uint32_t timeout) noexcept;
	static void MakeReady(TaskBase *t) noexcept;
}

namespace NotifyIndices
{
	constexpr uint32_t ReadWriteLocker = 0;		// RRFLibraries RTOSNotifyIndices.h
	constexpr uint32_t SbcInterface = 1;		// the SBC task waits for the next transfer (SbcInterface::ExchangeData)
	constexpr uint32_t Semaphore = 2;			// the stand-in of BinarySemaphore below
	constexpr size_t Count = 3;
}

// Stand-in for the TaskBase of RRFLibraries RTOSIface.h, as far as ReadWriteLock and this test use it
class TaskBase
{
public:
	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;
	enum class State { ready, blocked, finished };

	TaskBase(const char *p_name, unsigned int p_priority) noexcept : name(p_name), priority(p_priority) { }

	// xTaskNotifyGiveIndexed
	void Give(uint32_t index) noexcept
	{
		++notifyCount[index];
		if (state == State::blocked && waitingIndex == (int)index)
		{
			waitingIndex = -1;
			Cpu::MakeReady(this);
		}
	}

	// ulTaskNotifyTakeIndexed(index, pdTRUE, timeout): wait for a notification and clear the count, false if the wait timed out
	static bool TakeIndexed(uint32_t index, uint32_t timeout = TimeoutUnlimited) noexcept
	{
		TaskBase *const me = Cpu::running;
		if (me->notifyCount[index] == 0)
		{
			me->waitingIndex = (int)index;
			Cpu::Block(timeout);
			me->waitingIndex = -1;
			if (me->timedOut)
			{
				me->timedOut = false;
				return false;
			}
		}
		me->notifyCount[index] = 0;
		return true;
	}

	static TaskBase *GetCallerTaskHandle() noexcept { return Cpu::running; }

	const char *const name;
	const unsigned int priority;
	State state = State::ready;
	int waitingIndex = -1;						// the notification index that the task waits for
	bool waitHasTimeout = false;				// the task waits with a timeout
	bool timedOut = false;						// the timeout expired
	uint32_t notifyCount[NotifyIndices::Count] = { 0 };
	unsigned int hooksPassed = 0;
	std::function<void()> body;
	std::thread thread;
};

static void StopOnDeadlock() noexcept
{
	std::printf("FAIL  deadlock: no task can run;");
	for (const TaskBase *t : Cpu::tasks)
	{
		if (t->state == TaskBase::State::blocked)
		{
			std::printf(" %s is blocked", t->name);
		}
	}
	std::printf("\n");
	std::fflush(stdout);
	std::_Exit(1);								// the blocked threads cannot be joined
}

namespace Cpu
{
	// Hand the processor to 'next' and wait until this task gets it back, unless this task has finished
	static void SwitchTo(TaskBase *next) noexcept
	{
		std::unique_lock<std::mutex> lock(mutex);
		TaskBase *const me = running;
		running = next;
		cv.notify_all();
		if (me != nullptr && me != next && me->state != TaskBase::State::finished)
		{
			cv.wait(lock, [me] { return running == me; });
		}
	}

	// The highest priority ready task other than 'except'
	static TaskBase *Pick(const TaskBase *except) noexcept
	{
		TaskBase *best = nullptr;
		for (TaskBase *t : tasks)
		{
			if (t->state == TaskBase::State::ready && t != except && (best == nullptr || t->priority > best->priority))
			{
				best = t;
			}
		}
		return best;
	}

	// The running task has blocked or finished: run the next one. If none can run, the wait of a task that waits with a timeout expires.
	static void Reschedule() noexcept
	{
		TaskBase *const me = running;
		TaskBase *next = Pick(me);
		if (next == nullptr)
		{
			for (TaskBase *t : tasks)
			{
				if (t->state == TaskBase::State::blocked && t->waitHasTimeout)
				{
					t->timedOut = true;
					t->waitHasTimeout = false;
					t->state = TaskBase::State::ready;
					next = t;
					break;
				}
			}
		}
		if (next != nullptr)
		{
			SwitchTo(next);
			return;
		}
		for (const TaskBase *t : tasks)
		{
			if (t->state == TaskBase::State::blocked)
			{
				StopOnDeadlock();
			}
		}
		std::lock_guard<std::mutex> lock(mutex);
		allFinished = true;
		running = nullptr;
		cv.notify_all();
	}

	// A task became ready: it preempts the running task if its priority is higher, as soon as the scheduler is not suspended
	static void MaybePreempt() noexcept
	{
		if (suspended != 0)
		{
			yieldPending = true;
			return;
		}
		TaskBase *const me = running;
		TaskBase *const next = Pick(me);
		if (next != nullptr && next->priority > me->priority)
		{
			SwitchTo(next);
		}
	}

	static void Block(uint32_t timeout) noexcept
	{
		running->state = TaskBase::State::blocked;
		running->waitHasTimeout = (timeout != TaskBase::TimeoutUnlimited);
		Reschedule();
	}

	static void MakeReady(TaskBase *t) noexcept
	{
		if (t->state == TaskBase::State::blocked)
		{
			t->state = TaskBase::State::ready;
			t->waitHasTimeout = false;
		}
		MaybePreempt();
	}
}

// RRFLibraries RTOSIface.h: vTaskSuspendAll and xTaskResumeAll
namespace RTOSIface
{
	inline void EnterTaskCriticalSection() noexcept { ++Cpu::suspended; }

	inline bool LeaveTaskCriticalSection() noexcept
	{
		if (--Cpu::suspended == 0 && Cpu::yieldPending)
		{
			Cpu::yieldPending = false;
			Cpu::MaybePreempt();
		}
		return false;
	}
}

// A FreeRTOS binary semaphore with a timeout, as SbcInterface waits for the reply of the SBC task (fileSemaphore)
class BinarySemaphore
{
public:
	bool Take(uint32_t timeout) noexcept
	{
		if (!given)
		{
			waiter = Cpu::running;
			const bool ok = TaskBase::TakeIndexed(NotifyIndices::Semaphore, timeout);
			waiter = nullptr;
			if (!ok)
			{
				return false;
			}
		}
		given = false;
		return true;
	}

	void Give() noexcept
	{
		given = true;
		if (waiter != nullptr)
		{
			waiter->Give(NotifyIndices::Semaphore);
		}
	}

	void Reset() noexcept { given = false; waiter = nullptr; }

private:
	TaskBase *waiter = nullptr;
	bool given = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// Points where the work of the other task arrives. Each task counts the points it passes; the run that the test sets up lets the work
// arrive at one of them.

enum class HookAction { none, pollArrives, announcementArrives };

static TaskBase *hookTask = nullptr;
static unsigned int hookIndex = 0;
static HookAction hookAction = HookAction::none;
static TaskBase *sbcTask = nullptr;
static TaskBase *canReceiverTask = nullptr;
static bool pollPending = false;					// DSF has sent a live poll that the SBC task has not answered yet
static std::string actionPoint;						// where the work arrived in this run

static void Hook(const char *point) noexcept
{
	TaskBase *const me = Cpu::running;
	if (me != nullptr && me == hookTask && me->hooksPassed++ == hookIndex && hookAction != HookAction::none)
	{
		actionPoint = point;
		const HookAction action = hookAction;
		hookAction = HookAction::none;
		if (action == HookAction::pollArrives)
		{
			pollPending = true;
			sbcTask->Give(NotifyIndices::SbcInterface);		// the SBC task has the lower priority, so it runs when the CAN receiver waits
		}
		else
		{
			canReceiverTask->Give(NotifyIndices::SbcInterface);	// the CAN receiver has the higher priority, so it runs at once
		}
	}
}

// RRFLibraries FreelistManager.h: the stand-in allocates lock records with malloc directly
#define DECLARE_FREELIST_NEW_DELETE(_Type) \
	void *operator new(size_t sz) noexcept { return std::malloc(sz); } \
	void operator delete(void *p) noexcept { std::free(p); }

static unsigned int rtosAsserts = 0;

static void RtosAssertFailed(unsigned int line, const char *file) noexcept
{
	++rtosAsserts;
	Fail(std::string("RTOS_ASSERT failed at ") + file + ":" + std::to_string(line));
}

#define RTOS_ASSERT(_expr)	if (!(_expr)) { RtosAssertFailed(__LINE__, __FILE__); }		// as in RRFLibraries RTOSIface.cpp

// The real ReadWriteLock, ReadLocker and WriteLocker
struct LockRecord;
#include "RTOSIfaceClasses.inc"
#include "RTOSIface.inc"

// ---------------------------------------------------------------------------------------------------------------------------------------
// The SBC side: file operations of other tasks and the live poll of DSF

constexpr uint32_t SbcMaxRequestTime = 3000;		// src/SBC/SbcMessageFormats.h

static BinarySemaphore fileSemaphore;
static const char *pendingFileOp = nullptr;			// the file operation that the SBC task is asked to do
static std::vector<std::string> fileOpsDone, fileOpsTimedOut;
static bool pollAnswered = false;
static unsigned int reportedDrivers = 0;

// SbcInterface::DoFileOperation (src/SBC/SbcInterface.cpp): post the operation, which wakes the SBC task (PostFileOperation), and wait up
// to SbcMaxRequestTime for the SBC task to give the reply
static bool SbcFileOperation(const char *what) noexcept
{
	Hook(what);
	pendingFileOp = what;
	sbcTask->Give(NotifyIndices::SbcInterface);
	if (!fileSemaphore.Take(SbcMaxRequestTime))
	{
		pendingFileOp = nullptr;						// DoFileOperation clears fileOperation after the timeout
		fileOpsTimedOut.push_back(what);
		return false;
	}
	return true;
}

// A file on the SBC. FileStore::Open allocates a write buffer for a file opened for writing (src/Storage/FileStore.cpp), so Write only
// fills it. Truncate flushes and truncates the file, and Close closes it, and both wait for the SBC task
// (SbcInterface::TruncateFile and CloseFile in src/SBC/SbcInterface.cpp).
class FileStore
{
public:
	bool Write(const char *_ecv_array s) noexcept { Hook("FileStore::Write"); written += s; return true; }
	bool Truncate() noexcept { truncated = SbcFileOperation("FileStore::Truncate"); return truncated; }
	bool Close() noexcept { closed = SbcFileOperation("FileStore::Close"); return closed; }

	std::string written;
	bool truncated = false, closed = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// CAN and the expansion boards, as far as ProcessAnnouncement uses them

typedef uint8_t CanAddress;

namespace CanId
{
	constexpr CanAddress MaxCanAddress = 126;		// CANlib src/CanId.h
	constexpr CanAddress NoAddress = 255;
	constexpr CanAddress MainBoardAddress = 0;
}

constexpr size_t TextLengthAfterHeader(size_t dataLength, size_t headerLength) noexcept		// CANlib src/CanMessageFormats.h
{
	return (dataLength > headerLength) ? dataLength - headerLength : 0;
}

// CANlib src/CanMessageFormats.h
struct __attribute__((packed)) CanMessageAnnounceV0
{
	uint32_t timeSinceStarted;
	uint32_t numDrivers: 8,
			 zero : 24;
	char boardTypeAndFirmwareVersion[56];

	static size_t GetMaxTextLength(size_t dataLength) noexcept { return TextLengthAfterHeader(dataLength, 2 * sizeof(uint32_t)); }
};

struct __attribute__((packed)) CanMessageAnnounceV1
{
	uint32_t timeSinceStarted;
	uint8_t uniqueId[16];
	uint8_t numDrivers: 4,
			usesUf2Binary : 1,
			isReconnect : 1,
			wasShutDown : 1,
			noSmartDrivers : 1;
	char boardTypeAndFirmwareVersion[43];

	static size_t GetMaxTextLength(size_t dataLength) noexcept { return TextLengthAfterHeader(dataLength, sizeof(timeSinceStarted) + sizeof(uniqueId) + sizeof(uint8_t)); }
};

struct CanMessageAcknowledgeAnnounce { };

struct CanId_t
{
	CanAddress Src() const noexcept { return src; }
	CanAddress src;
};

union CanMessage
{
	CanMessageAnnounceV0 announceV0;
	CanMessageAnnounceV1 announceV1;
};

struct CanMessageBuffer
{
	template<class T> void SetupRequestMessageNoRid(CanAddress p_src, CanAddress p_dst) noexcept { acknowledged = true; }

	CanId_t id;
	size_t dataLength;
	CanMessage msg;
	bool acknowledged = false;
};

namespace CanInterface
{
	static CanAddress GetCanAddress() noexcept { return CanId::MainBoardAddress; }
	static void SendMessageNoReplyNoFree(CanMessageBuffer *buf) noexcept { Hook("CanInterface::SendMessageNoReplyNoFree"); }
}

// src/Platform/Event.h
enum class EventType : uint8_t { expansion_reconnect };

static std::vector<uint16_t> eventParams;

class Event
{
public:
	static bool AddEvent(EventType et, uint16_t p_param, CanAddress p_ba, uint8_t devNum, const char *_ecv_array format, ...) noexcept
	{
		Hook("Event::AddEvent");
		eventParams.push_back(p_param);
		return true;
	}
};

static uint32_t millis() noexcept { return 0; }

constexpr size_t StringLength100 = 100;

template<size_t N> class String
{
public:
	void copy(const char *_ecv_array s, size_t maxLength) noexcept { text.assign(s, strnlen(s, maxLength)); if (text.size() > N) { text.resize(N); } }
	const char *_ecv_array c_str() const noexcept { return text.c_str(); }
	size_t strlen() const noexcept { return text.size(); }

private:
	std::string text;
};

// src/RepRapFirmware.h
template <typename T> void DeleteArray(T *_ecv_array _ecv_null & ptr) noexcept
{
	T *_ecv_array _ecv_null p2 = nullptr;
	std::swap(ptr, p2);
	delete[] p2;
}

// src/CAN/ExpansionManager.h
enum class BoardState : uint8_t { unknown, flashing, flashFailed, resetting, running, timedOut };

class DriverData
{
public:
	void StoreIsSmartDriver(bool smart) noexcept { isSmart = smart; }
	bool isSmart = false;
};

struct UniqueId
{
	void SetFromRemote(const uint8_t *p) noexcept { valid = true; }
	void Clear() noexcept { valid = false; }
	bool valid = false;
};

struct ExpansionBoardData
{
	const char *_ecv_array _ecv_null typeName = nullptr;
	DriverData *_ecv_array _ecv_null driverData = nullptr;
	uint32_t whenLastStatusReportReceived = 0;
	uint32_t neverUsedRam = 0;
	UniqueId uniqueId;
	BoardState state = BoardState::unknown;
	uint8_t numDrivers = 0;
	bool hasVin = false, hasV12 = false, hasMcuTemp = false;
	bool usesUf2Binary = false;
};

class ExpansionManager
{
public:
	void ProcessAnnouncement(CanMessageBuffer *buf, bool isNewFormat) noexcept;

	static ReadWriteLock boardsLock;
	ExpansionBoardData boards[CanId::MaxCanAddress + 1];

private:
	void UpdateBoardState(CanAddress address, BoardState newState) noexcept
	{
		Hook((newState == BoardState::running) ? "ExpansionManager::UpdateBoardState to running" : "ExpansionManager::UpdateBoardState to unknown");
		boards[address].state = newState;
	}
};

ReadWriteLock ExpansionManager::boardsLock;

static ExpansionManager expansion;

// ---------------------------------------------------------------------------------------------------------------------------------------
// The accelerometer collection, as far as RemoteBoardRestarted uses it (src/Accelerometers/Accelerometers.cpp)

#define SUPPORT_ACCELEROMETERS	1

static FileStore *_ecv_null volatile accelerometerFile = nullptr;
static CanAddress expectedRemoteBoardAddress = CanId::NoAddress;
static unsigned int runsRecorded = 0, lastRunDataPoints = 0;

static void AddRemoteAccelerometerRun(CanAddress boardAddress, unsigned int numDataPoints) noexcept
{
	Hook("AddRemoteAccelerometerRun");
	++runsRecorded;
	lastRunDataPoints = numDataPoints;
}

namespace Accelerometers
{
	void RemoteBoardRestarted(CanAddress src) noexcept;
}

// The real text of the announcement and of the end of a collection on a restarted board
#include "ProcessAnnouncement.inc"
#include "RemoteBoardRestarted.inc"

// The live poll of DSF as far as it reports boards[].drivers: ObjectModel::ReportObjectModelArrayAsJson (src/ObjectModel/ObjectModel.cpp)
// holds the lock that the array table entry names while it reports the array
static void ReportBoardDrivers(CanAddress address) noexcept
{
	ReadLocker lock(ExpansionManager::boardsLock);
	Hook("SBC task: report of boards[].drivers");
	const ExpansionBoardData& board = expansion.boards[address];
	reportedDrivers = 0;
	for (size_t i = 0; i < board.numDrivers && board.driverData != nullptr; ++i)
	{
		(void)board.driverData[i].isSmart;
		++reportedDrivers;
	}
	Hook("SBC task: end of the report of boards[].drivers");
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Running the tasks

static void TaskEntry(TaskBase *t) noexcept
{
	{
		std::unique_lock<std::mutex> lock(Cpu::mutex);
		Cpu::cv.wait(lock, [t] { return Cpu::running == t; });
	}
	t->body();
	t->state = TaskBase::State::finished;
	Cpu::Reschedule();
}

// Run the tasks until all have finished. The first one starts; a task that starts blocked waits for Cpu::MakeReady.
static void RunTasks(const std::vector<TaskBase *>& tasks) noexcept
{
	Cpu::tasks = tasks;
	Cpu::allFinished = false;
	Cpu::suspended = 0;
	Cpu::yieldPending = false;
	for (TaskBase *t : tasks)
	{
		t->thread = std::thread(TaskEntry, t);
	}
	{
		std::unique_lock<std::mutex> lock(Cpu::mutex);
		Cpu::running = tasks.front();
		Cpu::cv.notify_all();
		Cpu::cv.wait(lock, [] { return Cpu::allFinished; });
	}
	for (TaskBase *t : tasks)
	{
		t->thread.join();
	}
	Cpu::tasks.clear();
}

static unsigned int failed = 0, passed = 0;

static void Report(const std::string& name, bool ok, const std::string& why) noexcept
{
	if (ok && failures.empty())
	{
		++passed;
		std::printf("PASS  %s\n", name.c_str());
		return;
	}
	++failed;
	std::printf("FAIL  %s\n", name.c_str());
	if (!ok)
	{
		std::printf("        %s\n", why.c_str());
	}
	for (const std::string& f : failures)
	{
		std::printf("        %s\n", f.c_str());
	}
}

// Whether nobody holds or waits for boardsLock, checked outside the tasks
static bool BoardsLockIsFree() noexcept
{
	TaskBase checker("checker", 0);
	Cpu::running = &checker;
	const bool free = ExpansionManager::boardsLock.ConditionalLockForWriting();
	if (free)
	{
		ExpansionManager::boardsLock.ReleaseWriter();
	}
	Cpu::running = nullptr;
	return free;
}

enum class Announcement { restartV1, restartV0, reconnect };

static const char *Name(Announcement a) noexcept
{
	return (a == Announcement::restartV1) ? "restarted board (announceV1)"
			: (a == Announcement::restartV0) ? "restarted board (announceV0)"
				: "board that only regained time sync (announceV1 with isReconnect)";
}

constexpr CanAddress BoardAddress = 20;
static const char OldFirmware[] = "TOOL1LC|3.7.0+1.mp.5";
static const char NewFirmware[] = "TOOL1LC|3.7.0+1.mp.6";
constexpr unsigned int NumDrivers = 1;

// SBC mode. Board 20 runs a remote accelerometer collection into a file on the SBC (M956), and its announcement arrives at the CAN
// receiver (priority 6, the SBC task has 2). With 'pollFirst' the SBC task is answering a live poll of DSF when the announcement arrives
// at one of the SBC task's points; otherwise the CAN receiver starts and the poll arrives at one of its points. With 'newFirmware' the board
// announces another firmware version, so ProcessAnnouncement allocates new driver data. Returns the number of points that the task passes
// at which the other one's work arrives.
static unsigned int RunAnnouncement(Announcement kind, bool collectionRunning, bool newFirmware, bool pollFirst, unsigned int arrivalPoint, bool countOnly) noexcept
{
	failures.clear();
	actionPoint.clear();
	fileOpsDone.clear();
	fileOpsTimedOut.clear();
	eventParams.clear();
	fileSemaphore.Reset();
	pendingFileOp = nullptr;
	pollAnswered = false;
	reportedDrivers = 0;
	runsRecorded = 0;
	lastRunDataPoints = 99;
	rtosAsserts = 0;

	ExpansionBoardData& board = expansion.boards[BoardAddress];
	char *const oldTypeName = new char[sizeof(OldFirmware)];
	std::strcpy(oldTypeName, OldFirmware);
	board = ExpansionBoardData();
	board.typeName = oldTypeName;
	board.numDrivers = NumDrivers;
	board.driverData = new DriverData[NumDrivers];
	board.state = BoardState::running;

	FileStore file;
	accelerometerFile = (collectionRunning) ? &file : nullptr;
	expectedRemoteBoardAddress = (collectionRunning) ? BoardAddress : CanId::NoAddress;

	CanMessageBuffer buf;
	std::memset(&buf.msg, 0, sizeof(buf.msg));
	buf.id.src = BoardAddress;
	const char *const firmware = (newFirmware) ? NewFirmware : OldFirmware;
	const bool isNewFormat = (kind != Announcement::restartV0);
	if (isNewFormat)
	{
		buf.msg.announceV1.numDrivers = NumDrivers;
		buf.msg.announceV1.isReconnect = (kind == Announcement::reconnect);
		std::strncpy(buf.msg.announceV1.boardTypeAndFirmwareVersion, firmware, sizeof(buf.msg.announceV1.boardTypeAndFirmwareVersion));
		buf.dataLength = sizeof(uint32_t) + 16 + 1 + std::strlen(firmware);
	}
	else
	{
		buf.msg.announceV0.numDrivers = NumDrivers;
		std::strncpy(buf.msg.announceV0.boardTypeAndFirmwareVersion, firmware, sizeof(buf.msg.announceV0.boardTypeAndFirmwareVersion));
		buf.dataLength = 2 * sizeof(uint32_t) + std::strlen(firmware);
	}

	TaskBase sbc("SBC", 2), canReceiver("CanReceiver", 6);
	sbcTask = &sbc;
	canReceiverTask = &canReceiver;
	bool canReceiverDone = false;
	pollPending = pollFirst;
	hookTask = (pollFirst) ? &sbc : &canReceiver;
	hookIndex = arrivalPoint;
	hookAction = (countOnly) ? HookAction::none : (pollFirst) ? HookAction::announcementArrives : HookAction::pollArrives;

	canReceiver.body = [&]
	{
		if (pollFirst)
		{
			(void)TaskBase::TakeIndexed(NotifyIndices::SbcInterface);		// wait for the announcement
		}
		expansion.ProcessAnnouncement(&buf, isNewFormat);
		canReceiverDone = true;
		sbc.Give(NotifyIndices::SbcInterface);
	};
	sbc.body = [&]
	{
		if (!pollFirst)
		{
			(void)TaskBase::TakeIndexed(NotifyIndices::SbcInterface);		// wait for the next transfer
		}
		for (;;)
		{
			// SbcInterface::ExchangeData: first the requests that DSF sent, then the file operations of other tasks
			if (pollPending)
			{
				pollPending = false;
				ReportBoardDrivers(BoardAddress);
				pollAnswered = true;
				if (pollFirst && !canReceiverDone && canReceiver.state == TaskBase::State::blocked && canReceiver.waitingIndex == (int)NotifyIndices::SbcInterface)
				{
					canReceiver.Give(NotifyIndices::SbcInterface);			// the announcement arrives after the poll, in the count run and after the last point
				}
			}
			if (pendingFileOp != nullptr)
			{
				fileOpsDone.push_back(pendingFileOp);
				pendingFileOp = nullptr;
				fileSemaphore.Give();
			}
			if (canReceiverDone && !pollPending && pendingFileOp == nullptr)
			{
				break;
			}
			(void)TaskBase::TakeIndexed(NotifyIndices::SbcInterface);		// wait for the next transfer
		}
	};

	if (pollFirst)
	{
		canReceiver.state = TaskBase::State::blocked;						// waiting for a CAN message
		canReceiver.waitingIndex = NotifyIndices::SbcInterface;
		RunTasks({ &sbc, &canReceiver });
	}
	else
	{
		sbc.state = TaskBase::State::blocked;								// waiting for the next transfer
		sbc.waitingIndex = NotifyIndices::SbcInterface;
		RunTasks({ &canReceiver, &sbc });
	}
	hookTask = sbcTask = canReceiverTask = nullptr;

	const unsigned int numPoints = (pollFirst) ? sbc.hooksPassed : canReceiver.hooksPassed;
	if (!countOnly)
	{
		std::string name = std::string(Name(kind)) + ((collectionRunning) ? ", collection running" : ", no collection")
							+ ((newFirmware) ? ", new firmware version" : "")
							+ ((pollFirst) ? ", the announcement arrives at the SBC task's point '" : ", a live poll of DSF arrives at the CAN receiver's point '")
							+ actionPoint + "'";
		std::string why;
		bool ok = true;
		if (!fileOpsTimedOut.empty())
		{
			ok = false;
			for (const std::string& op : fileOpsTimedOut)
			{
				why += ((why.empty()) ? "" : " and ") + op;
			}
			why += " waited SbcMaxRequestTime for the SBC task";
		}
		const bool restarted = (kind != Announcement::reconnect);
		if (ok && restarted && collectionRunning)
		{
			if (file.written != "Board restarted before the collection was complete\n" || !file.truncated || !file.closed || accelerometerFile != nullptr
				|| runsRecorded != 1 || lastRunDataPoints != 0)
			{
				ok = false;
				why = "the accelerometer file was not finished as RemoteBoardRestarted does it";
			}
		}
		if (ok && !(restarted && collectionRunning))
		{
			if (!fileOpsDone.empty() || !file.written.empty() || runsRecorded != 0 || accelerometerFile != ((collectionRunning) ? &file : nullptr))
			{
				ok = false;
				why = "the accelerometer file was touched although the board did not restart during a collection";
			}
		}
		if (ok && !pollAnswered)
		{
			ok = false;
			why = "the live poll was not answered";
		}
		if (ok && (board.state != BoardState::running || board.typeName == nullptr || std::strcmp(board.typeName, firmware) != 0
					|| board.numDrivers != NumDrivers || board.driverData == nullptr || !buf.acknowledged))
		{
			ok = false;
			why = "the board data were not updated or the announcement was not acknowledged";
		}
		if (ok && reportedDrivers != NumDrivers)
		{
			ok = false;
			why = "the poll reported " + std::to_string(reportedDrivers) + " drivers instead of " + std::to_string(NumDrivers);
		}
		if (ok && !BoardsLockIsFree())
		{
			ok = false;
			why = "boardsLock is still held or waited for";
		}
		Report(name, ok, why);
	}

	if (board.typeName != oldTypeName)
	{
		delete[] board.typeName;			// ProcessAnnouncement keeps a type name that another board may share, so the old one is not freed there
	}
	delete[] oldTypeName;
	board.typeName = nullptr;
	DeleteArray(board.driverData);
	accelerometerFile = nullptr;
	return numPoints;
}

int main()
{
	for (Announcement kind : { Announcement::restartV1, Announcement::restartV0, Announcement::reconnect })
	{
		for (bool collectionRunning : { true, false })
		{
			for (bool newFirmware : { false, true })
			{
				for (bool pollFirst : { false, true })
				{
					const unsigned int numPoints = RunAnnouncement(kind, collectionRunning, newFirmware, pollFirst, 0, true);
					for (unsigned int point = 0; point < numPoints; ++point)
					{
						(void)RunAnnouncement(kind, collectionRunning, newFirmware, pollFirst, point, false);
					}
				}
			}
		}
	}

	std::printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
