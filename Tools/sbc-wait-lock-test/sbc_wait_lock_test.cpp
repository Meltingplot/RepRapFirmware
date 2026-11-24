// Host test of the rule that no task waits for the SBC task while it holds a ReadWriteLock for writing, in SBC mode.
//
// The SBC task serves the file operations and macro requests of the other tasks (SbcInterface::DoFileOperation, GCodeBuffer::RequestMacroFile)
// and answers the requests of DSF, for which it takes locks of the object model: the live poll reads objects under the read lock that their
// table entry names (ObjectModel::ReportObjectModelArrayAsJson, GlobalVariables::ReportAsJson), and SetVariable takes the global variables
// for writing and evaluates its expression, which takes Heap::heapLock for writing (case SbcRequest::SetVariable in src/SBC/SbcInterface.cpp).
// A task that waits for the SBC task while it holds such a lock stops the SBC task until its own request has timed out after
// SbcMaxRequestTime. SbcInterface::WaitForFileSlot and GCodeBuffer::RequestMacroFile therefore report an internal error if the calling task
// holds a write lock, which RRFLibraries counts per task (TaskBase::GetCallerWriteLocksHeld).
//
// Sections:
//  1. the count of write locks in the real ReadWriteLock of RRFLibraries
//  2. why the rule holds: a wait for the SBC task under a write lock times out, and the check reports it
//  3. the announcement of a restarted expansion board, which finished an accelerometer file under ExpansionManager::boardsLock (C1)
//  4. fileexists() of a string on the heap, which waited under Heap::heapLock for reading (C5, not a write lock, so it has its own check)
//  5. the commands of text that call fileexists() or fileread(), which DSF evaluates instead (C2/C3): the scanner and the hand-over of
//     global and set global commands in StringParser::ProcessConditionalGCode. The hand-over of other commands in GCodes::ActOnCode is not
//     covered here.
//
// The test compiles the real text of these functions, which the Makefile extracts at build time. It runs the tasks as threads on one
// simulated processor: exactly one task runs at a time, and a task runs until it blocks unless a task with a higher priority becomes ready.
// A wait with a timeout expires when no task can run any more.

#define RTOS							// compile the members that RRFLibraries declares only for an RTOS
#define HAS_SBC_INTERFACE	1

#include <atomic>
#include <cctype>
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
#include <set>
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
#define pre(x)
#define THROWS(...)

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
	constexpr uint32_t SbcInterface = 1;		// the SBC task waits for the next transfer
	constexpr uint32_t Semaphore = 2;			// the stand-in of BinarySemaphore below
	constexpr uint32_t Start = 3;				// a task of the test waits for its turn
	constexpr size_t Count = 4;
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
			if (timeout == 0)
			{
				return false;
			}
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

	// The member that the change in RRFLibraries adds, as it is declared there
	static unsigned int GetCallerWriteLocksHeld() noexcept { return _ecv_not_null(GetCallerTaskHandle())->writeLocksHeld; }
	unsigned int writeLocksHeld = 0;

	const char *const name;
	const unsigned int priority;
	State state = State::ready;
	int waitingIndex = -1;						// the notification index that the task waits for
	bool waitHasTimeout = false;				// the task waits with a timeout
	bool timedOut = false;						// the timeout expired
	uint32_t notifyCount[NotifyIndices::Count] = { 0 };
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

// A FreeRTOS binary semaphore with a timeout, as SbcInterface waits for the SBC task (fileSemaphore, asyncWriteSemaphore)
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

// Only one task at a time does file operations in this test, so the file mutex never waits
class Mutex { };

class MutexLocker
{
public:
	explicit MutexLocker(Mutex& m) noexcept { }
};

// RRFLibraries FreelistManager.h: the stand-in allocates lock records with malloc directly
#define DECLARE_FREELIST_NEW_DELETE(_Type) \
	void *operator new(size_t sz) noexcept { return std::malloc(sz); } \
	void operator delete(void *p) noexcept { std::free(p); }

static void RtosAssertFailed(unsigned int line, const char *file) noexcept
{
	Fail(std::string("RTOS_ASSERT failed at ") + file + ":" + std::to_string(line));
}

#define RTOS_ASSERT(_expr)	if (!(_expr)) { RtosAssertFailed(__LINE__, __FILE__); }		// as in RRFLibraries RTOSIface.cpp

// The real ReadWriteLock, its lockers and ReadLockedPointer
struct LockRecord;
#include "RTOSIfaceClasses.inc"
#include "RTOSIface.inc"

// ---------------------------------------------------------------------------------------------------------------------------------------
// Strings, as far as the extracted functions use them (RRFLibraries General/StringRef.h and StringFunctions.h, CoreN2G Core.h)

class StringRef
{
public:
	StringRef(char *p, size_t len) noexcept : p(p), len(len) { }
	void copy(const char *s) noexcept { std::snprintf(p, len, "%s", s); }
	void cat(const char *s) noexcept { const size_t n = std::strlen(p); std::snprintf(p + n, len - n, "%s", s); }

private:
	char *p;
	size_t len;
};

template<size_t N> class String
{
public:
	String() noexcept { buf[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(buf, N + 1); }
	const char *c_str() const noexcept { return buf; }
	void copy(const char *s, size_t maxLength) noexcept { std::snprintf(buf, sizeof(buf), "%.*s", (int)strnlen(s, maxLength), s); }
	size_t strlen() const noexcept { return std::strlen(buf); }

private:
	char buf[N + 1];
};

constexpr size_t MaxFilenameLength = 120;
constexpr size_t StringLength100 = 100;

static bool StringStartsWith(const char *string, const char *starting) noexcept
{
	return std::strncmp(string, starting, std::strlen(starting)) == 0;
}

static inline bool isAlnum(char c) noexcept
{
	return isalnum((int)c) != 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The SBC interface, as far as the other tasks wait for the SBC task. The file functions are the real ones.

constexpr uint32_t SbcMaxRequestTime = 3000;		// src/SBC/SbcMessageFormats.h
typedef uint32_t FileHandle;
enum MessageType { ErrorMessage };

static TaskBase *sbcTask = nullptr;					// src/SBC/SbcInterface.cpp
static std::vector<std::string> messages;			// messages that the real functions report
static std::vector<std::string> internalErrors;		// REPORT_INTERNAL_ERROR, with the function that reported it

class SbcInterface
{
public:
	bool IsConnected() const noexcept { return connected; }

	bool FileExists(const char *filename) noexcept;
	bool TruncateFile(FileHandle handle) noexcept;
	void CloseFile(FileHandle handle) noexcept;

	void Reset() noexcept
	{
		connected = true;
		fileOperation = FileOperation::none;
		fileOperationPending = false;
		fileSuccess = false;
		delaying = false;
		fileSemaphore.Reset();
		asyncWriteSemaphore.Reset();
	}

	bool connected = true;

	// SbcInterface.h
	Mutex fileMutex;
	BinarySemaphore fileSemaphore;
	BinarySemaphore asyncWriteSemaphore;
	enum class FileOperation { none, checkFileExists, writeAsync, truncate, close } fileOperation = FileOperation::none;
	std::atomic<bool> fileOperationPending { false };
	const char *filePath = nullptr;
	FileHandle fileHandle = 0;
	std::atomic<bool> fileSuccess { false };
	std::atomic<bool> delaying { false };

	bool WaitForFileSlot() noexcept;
	void PostFileOperation(FileOperation f) noexcept;
	bool DoFileOperation(FileOperation f) noexcept;
};

class StringHandle;

class Platform
{
public:
	void Message(MessageType type, const char *s) noexcept { Record(s); }
	void MessageF(MessageType type, const char *fmt, ...) noexcept
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		Record(buf);
	}

	static void Record(std::string s) noexcept
	{
		while (!s.empty() && s.back() == '\n')
		{
			s.pop_back();
		}
		messages.push_back(s);
	}

	bool SysFileExists(const char *_ecv_array filename) const noexcept;
	bool SysFileExists(const StringHandle& filename) const noexcept;

	// Platform::MakeSysFileName in src/Platform/Platform.cpp combines the name with the system directory
	bool MakeSysFileName(const StringRef& result, const char *_ecv_array filename) const noexcept
	{
		StringRef r = result;
		r.copy("0:/sys/");
		r.cat(filename);
		return true;
	}
};

class RepRap
{
public:
	bool UsingSbcInterface() const noexcept { return usingSbc; }
	SbcInterface& GetSbcInterface() noexcept { return sbc; }
	Platform& GetPlatform() noexcept { return platform; }
	void ReportInternalError(const char *file, const char *func, int line) noexcept { internalErrors.push_back(func); }

	bool usingSbc = true;
	SbcInterface sbc;
	Platform platform;
};

static RepRap reprap;

#define REPORT_INTERNAL_ERROR do { reprap.ReportInternalError((__FILE__), (__func__), (__LINE__)); } while(0)		// src/RepRapFirmware.h

#include "SbcInterface.inc"

// src/Storage/MassStorage.cpp: in SBC mode the SBC answers
namespace MassStorage
{
	static bool FileExists(const char *_ecv_array filePath) noexcept { return reprap.GetSbcInterface().FileExists(filePath); }
}

// A file on the SBC that is open for writing. FileStore::Open allocates a write buffer for it (src/Storage/FileStore.cpp), so Write only
// fills it, while Truncate and Close go to the SBC (FileStore::Truncate and FileStore::Close in src/Storage/FileStore.cpp)
class FileStore
{
public:
	bool Write(const char *_ecv_array s) noexcept { written += s; return true; }
	bool Truncate() noexcept { truncated = reprap.GetSbcInterface().TruncateFile(handle); return truncated; }
	bool Close() noexcept { reprap.GetSbcInterface().CloseFile(handle); closed = true; return true; }

	FileHandle handle = 7;
	std::string written;
	bool truncated = false, closed = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The SBC task: first the requests of DSF, then the file operations of the other tasks (SbcInterface::ExchangeData)

struct DsfRequest
{
	const char *what;
	ReadWriteLock *lock;
	bool forWriting;					// SetVariable takes the global variables and, for a string, Heap::heapLock for writing
	std::function<void()> work;			// what the SBC task does under the lock, if anything
};

static std::vector<DsfRequest> dsfRequests;
static std::vector<std::string> dsfRequestsAnswered, fileOpsServed;
static std::set<std::string> filesOnSbc;
static bool otherTasksDone = false;

static void SbcTaskBody() noexcept
{
	SbcInterface& sbc = reprap.GetSbcInterface();
	for (;;)
	{
		while (!dsfRequests.empty())
		{
			const DsfRequest request = dsfRequests.front();
			dsfRequests.erase(dsfRequests.begin());
			if (request.forWriting)
			{
				WriteLocker lock(*request.lock);
				if (request.work) { request.work(); }
			}
			else
			{
				ReadLocker lock(*request.lock);
				if (request.work) { request.work(); }
			}
			dsfRequestsAnswered.push_back(request.what);
		}
		if (sbc.fileOperationPending)
		{
			switch (sbc.fileOperation)
			{
			case SbcInterface::FileOperation::checkFileExists:
				sbc.fileSuccess = filesOnSbc.count(sbc.filePath) != 0;
				fileOpsServed.push_back(std::string("checkFileExists ") + sbc.filePath);
				break;
			case SbcInterface::FileOperation::truncate:
				sbc.fileSuccess = true;
				fileOpsServed.push_back("truncate");
				break;
			case SbcInterface::FileOperation::close:
				sbc.fileSuccess = true;
				fileOpsServed.push_back("close");
				break;
			default:
				Fail("the SBC task got an unexpected file operation");
				break;
			}
			sbc.fileOperation = SbcInterface::FileOperation::none;
			sbc.fileOperationPending = false;
			sbc.fileSemaphore.Give();
		}
		if (otherTasksDone && dsfRequests.empty() && !sbc.fileOperationPending)
		{
			break;
		}
		sbc.delaying = true;
		(void)TaskBase::TakeIndexed(NotifyIndices::SbcInterface);		// wait for the next transfer
		sbc.delaying = false;
	}
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

// Run 'body' as a task with the given priority, together with the SBC task, which waits for its first transfer
static void RunWithSbcTask(const char *name, unsigned int priority, const std::function<void()>& body) noexcept
{
	TaskBase task(name, priority), sbc("SBC", 2);
	sbcTask = &sbc;
	otherTasksDone = false;
	task.body = [&] { body(); otherTasksDone = true; sbc.Give(NotifyIndices::SbcInterface); };
	sbc.body = SbcTaskBody;
	sbc.state = TaskBase::State::blocked;
	sbc.waitingIndex = NotifyIndices::SbcInterface;
	reprap.GetSbcInterface().delaying = true;
	RunTasks({ &task, &sbc });
	sbcTask = nullptr;
}

static void ResetRun() noexcept
{
	failures.clear();
	messages.clear();
	internalErrors.clear();
	dsfRequests.clear();
	dsfRequestsAnswered.clear();
	fileOpsServed.clear();
	filesOnSbc.clear();
	reprap.usingSbc = true;
	reprap.GetSbcInterface().Reset();
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

static std::string Join(const std::vector<std::string>& v) noexcept
{
	std::string s;
	for (const std::string& e : v)
	{
		s += (s.empty() ? "" : ", ") + e;
	}
	return (s.empty()) ? "none" : s;
}

// Whether nobody holds or waits for the lock, checked outside the tasks
static bool IsFree(ReadWriteLock& lock) noexcept
{
	TaskBase checker("checker", 0);
	Cpu::running = &checker;
	const bool free = lock.ConditionalLockForWriting();
	if (free)
	{
		lock.ReleaseWriter();
	}
	Cpu::running = nullptr;
	return free;
}

static void Expect(bool cond, const std::string& what) noexcept
{
	if (!cond)
	{
		Fail(what);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 1. The count of write locks

static ReadWriteLock lockA, lockB;

static void TestCountOneTask(const char *name, const std::function<void()>& body) noexcept
{
	ResetRun();
	TaskBase task("Main", 1);
	task.body = body;
	RunTasks({ &task });
	Expect(task.writeLocksHeld == 0, "the task still counts " + std::to_string(task.writeLocksHeld) + " write locks at its end");
	Expect(IsFree(lockA) && IsFree(lockB), "a lock is still held");
	Report(std::string("write lock count: ") + name, true, "");
}

static void ExpectCount(unsigned int n, const char *where) noexcept
{
	const unsigned int count = TaskBase::GetCallerWriteLocksHeld();
	Expect(count == n, std::string(where) + ": count " + std::to_string(count) + ", expected " + std::to_string(n));
}

static void TestWriteLockCount() noexcept
{
	TestCountOneTask("a write lock counts while it is held", []
	{
		ExpectCount(0, "before");
		lockA.LockForWriting();
		ExpectCount(1, "after LockForWriting");
		lockA.ReleaseWriter();
		ExpectCount(0, "after ReleaseWriter");
	});

	TestCountOneTask("nested write locks and a read lock under the own write lock", []
	{
		lockA.LockForWriting();
		lockA.LockForWriting();
		ExpectCount(2, "after the nested LockForWriting");
		lockA.LockForReading();
		ExpectCount(2, "after LockForReading under the own write lock");
		lockA.ReleaseReader();
		lockA.ReleaseWriter();
		ExpectCount(1, "after the inner ReleaseWriter");
		lockA.ReleaseWriter();
		ExpectCount(0, "after the outer ReleaseWriter");
	});

	TestCountOneTask("write locks on two locks", []
	{
		WriteLocker a(lockA);
		{
			WriteLocker b(lockB);
			ExpectCount(2, "with both");
		}
		ExpectCount(1, "after WriteLocker of B ended");
	});

	TestCountOneTask("read locks do not count", []
	{
		ReadLocker a(lockA);
		ReadLocker b(lockB);
		ExpectCount(0, "with two read locks");
	});

	TestCountOneTask("ConditionalLockForWriting counts when it succeeds", []
	{
		Expect(lockA.ConditionalLockForWriting(), "ConditionalLockForWriting failed on a free lock");
		ExpectCount(1, "after ConditionalLockForWriting");
		lockA.ReleaseWriter();
		ExpectCount(0, "after ReleaseWriter");
		lockA.LockForReading();
		Expect(!lockA.ConditionalLockForWriting(), "ConditionalLockForWriting succeeded on a read-locked lock");
		ExpectCount(0, "after ConditionalLockForWriting failed");
		lockA.ReleaseReader();
	});

	TestCountOneTask("a downgraded write lock no longer counts", []
	{
		WriteLocker a(lockA);
		ExpectCount(1, "before Downgrade");
		a.Downgrade();
		ExpectCount(0, "after Downgrade");
	});

	// A task with a higher priority waits for the write lock that a task with a lower priority holds: it counts the lock once it gets it
	ResetRun();
	{
		TaskBase low("Low", 1), high("High", 3);
		low.body = [&]
		{
			lockA.LockForWriting();
			high.Give(NotifyIndices::Start);								// high runs at once and waits for the lock
			Expect(high.state == TaskBase::State::blocked, "the task with the higher priority does not wait for the lock");
			Expect(high.writeLocksHeld == 0, "a task that waits for a write lock counts it");
			ExpectCount(1, "the owner while the other task waits");
			lockA.ReleaseWriter();											// passes the lock on, and high preempts
			ExpectCount(0, "the former owner");
		};
		high.body = [&]
		{
			(void)TaskBase::TakeIndexed(NotifyIndices::Start);
			lockA.LockForWriting();
			ExpectCount(1, "the task that waited, once it has the lock");
			lockA.ReleaseWriter();
			ExpectCount(0, "the task that waited, after ReleaseWriter");
		};
		high.state = TaskBase::State::blocked;
		high.waitingIndex = NotifyIndices::Start;
		RunTasks({ &low, &high });
		Expect(low.writeLocksHeld == 0 && high.writeLocksHeld == 0, "a count is not zero at the end");
		Expect(IsFree(lockA), "lock A is still held");
		Report("write lock count: a lock handed over to a waiting task counts for that task", true, "");
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 2. Why the rule holds. Main (priority 1) checks whether a file exists on the SBC while DSF has a request pending that needs lock A.

static ReadWriteLock objectLock;

enum class MainHolds { nothing, readLock, writeLock };

static void TestWaitUnderLock(MainHolds holds, bool dsfWrites) noexcept
{
	ResetRun();
	filesOnSbc.insert("0:/sys/a.g");
	dsfRequests.push_back(DsfRequest { (dsfWrites) ? "SetVariable" : "live poll", &objectLock, dsfWrites, nullptr });
	bool exists = false;
	RunWithSbcTask("Main", 1, [&]
	{
		if (holds == MainHolds::writeLock)
		{
			WriteLocker lock(objectLock);
			exists = reprap.GetPlatform().SysFileExists("a.g");
		}
		else if (holds == MainHolds::readLock)
		{
			ReadLocker lock(objectLock);
			exists = reprap.GetPlatform().SysFileExists("a.g");
		}
		else
		{
			exists = reprap.GetPlatform().SysFileExists("a.g");
		}
	});

	const char *const holdsText = (holds == MainHolds::writeLock) ? "a write lock" : (holds == MainHolds::readLock) ? "a read lock" : "no lock";
	const std::string name = std::string("why: Main waits for the SBC task with ") + holdsText + " that a pending " + ((dsfWrites) ? "SetVariable needs for writing" : "live poll needs for reading");
	const bool conflict = (holds == MainHolds::writeLock) || (holds == MainHolds::readLock && dsfWrites);
	bool ok = true;
	std::string why;
	if (conflict)
	{
		// The SBC task waits for Main's lock, so Main's request times out
		ok = !exists && !messages.empty() && messages.front().find("Timeout") != std::string::npos;
		why = "expected the request of Main to time out, messages: " + Join(messages);
	}
	else
	{
		ok = exists && messages.empty();
		why = "expected the file to be found without a timeout, messages: " + Join(messages);
	}
	if (ok && dsfRequestsAnswered.size() != 1)
	{
		ok = false;
		why = "the request of DSF was not answered";
	}
	const bool checkFires = (holds == MainHolds::writeLock);
	if (ok && (internalErrors.size() != 0) != checkFires)
	{
		ok = false;
		why = std::string("WaitForFileSlot ") + ((checkFires) ? "did not report" : "reported") + " an internal error: " + Join(internalErrors);
	}
	if (ok && !IsFree(objectLock))
	{
		ok = false;
		why = "the lock is still held";
	}
	Report(name, ok, why);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 3. The announcement of an expansion board that restarted during a remote accelerometer collection (C1)

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
	static void SendMessageNoReplyNoFree(CanMessageBuffer *buf) noexcept { }
}

// src/Platform/Event.h
enum class EventType : uint8_t { expansion_reconnect };

class Event
{
public:
	static bool AddEvent(EventType et, uint16_t p_param, CanAddress p_ba, uint8_t devNum, const char *_ecv_array format, ...) noexcept { return true; }
};

static uint32_t millis() noexcept { return 0; }

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
	void UpdateBoardState(CanAddress address, BoardState newState) noexcept { boards[address].state = newState; }
};

ReadWriteLock ExpansionManager::boardsLock;

static ExpansionManager expansion;

// The accelerometer collection, as far as RemoteBoardRestarted uses it (src/Accelerometers/Accelerometers.cpp)
#define SUPPORT_ACCELEROMETERS	1

static FileStore *_ecv_null volatile accelerometerFile = nullptr;
static CanAddress expectedRemoteBoardAddress = CanId::NoAddress;
static unsigned int runsRecorded = 0, lastRunDataPoints = 0;

static void AddRemoteAccelerometerRun(CanAddress boardAddress, unsigned int numDataPoints) noexcept
{
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

// The CAN receiver (priority 6) handles the announcement of board 20, which runs a remote accelerometer collection into a file on the SBC
// (M956). A live poll of DSF that reports boards[].drivers is pending; its array table entry names boardsLock (objectModelArrayTable in
// src/CAN/ExpansionManager.cpp).
static void TestAnnouncement(Announcement kind, bool collectionRunning, bool newFirmware) noexcept
{
	ResetRun();
	runsRecorded = 0;
	lastRunDataPoints = 99;

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

	// The live poll as far as it reports boards[].drivers: ObjectModel::ReportObjectModelArrayAsJson (src/ObjectModel/ObjectModel.cpp)
	// holds the lock of the array table entry while it reports the array
	unsigned int reportedDrivers = 0;
	dsfRequests.push_back(DsfRequest { "live poll of boards[].drivers", &ExpansionManager::boardsLock, false, [&reportedDrivers]
		{
			const ExpansionBoardData& b = expansion.boards[BoardAddress];
			for (size_t i = 0; i < b.numDrivers && b.driverData != nullptr; ++i)
			{
				(void)b.driverData[i].isSmart;
				++reportedDrivers;
			}
		} });

	RunWithSbcTask("CanReceiver", 6, [&] { expansion.ProcessAnnouncement(&buf, isNewFormat); });

	const std::string name = std::string("C1: ") + Name(kind) + ((collectionRunning) ? ", collection running" : ", no collection")
								+ ((newFirmware) ? ", new firmware version" : "");
	bool ok = true;
	std::string why;
	if (!internalErrors.empty())
	{
		ok = false;
		why = "WaitForFileSlot reported an internal error: the CAN receiver waited for the SBC task while it held a write lock; messages: " + Join(messages);
	}
	const bool restarted = (kind != Announcement::reconnect);
	if (ok && restarted && collectionRunning
		&& (file.written != "Board restarted before the collection was complete\n" || !file.truncated || !file.closed || !messages.empty()
			|| accelerometerFile != nullptr || runsRecorded != 1 || lastRunDataPoints != 0))
	{
		ok = false;
		why = "the accelerometer file was not finished as RemoteBoardRestarted does it; messages: " + Join(messages);
	}
	if (ok && !(restarted && collectionRunning)
		&& (!fileOpsServed.empty() || !file.written.empty() || runsRecorded != 0 || accelerometerFile != ((collectionRunning) ? &file : nullptr)))
	{
		ok = false;
		why = "the accelerometer file was touched although the board did not restart during a collection";
	}
	if (ok && (dsfRequestsAnswered.size() != 1 || reportedDrivers != NumDrivers))
	{
		ok = false;
		why = "the live poll was not answered or reported " + std::to_string(reportedDrivers) + " drivers";
	}
	if (ok && (board.state != BoardState::running || board.typeName == nullptr || std::strcmp(board.typeName, firmware) != 0
				|| board.numDrivers != NumDrivers || board.driverData == nullptr || !buf.acknowledged))
	{
		ok = false;
		why = "the board data were not updated or the announcement was not acknowledged";
	}
	if (ok && !IsFree(ExpansionManager::boardsLock))
	{
		ok = false;
		why = "boardsLock is still held or waited for";
	}
	Report(name, ok, why);

	if (board.typeName != oldTypeName)
	{
		delete[] board.typeName;			// ProcessAnnouncement keeps a type name that another board may share, so the old one is not freed there
	}
	delete[] oldTypeName;
	board.typeName = nullptr;
	DeleteArray(board.driverData);
	accelerometerFile = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 4. fileexists() of a string on the heap (C5). ExpressionParser hands the string to Platform::SysFileExists; StringHandle::Get takes
// Heap::heapLock for reading (src/Platform/StringHandle.cpp). A SetVariable of DSF with a string value takes it for writing in the SBC task.

namespace Heap
{
	static ReadWriteLock heapLock;
}

class StringHandle
{
public:
	explicit StringHandle(const char *s) noexcept : text(s) { }
	ReadLockedPointer<const char> Get() const noexcept { return ReadLockedPointer<const char>(Heap::heapLock, text); }

private:
	const char *text;
};

#include "SysFileExists.inc"

static void TestFileExistsOfHeapString(bool setVariablePending) noexcept
{
	ResetRun();
	filesOnSbc.insert("0:/sys/heap.g");
	if (setVariablePending)
	{
		dsfRequests.push_back(DsfRequest { "SetVariable of a string", &Heap::heapLock, true, nullptr });
	}
	bool exists = false, missing = true;
	const StringHandle present("heap.g"), absent("other.g");
	RunWithSbcTask("Main", 1, [&]
	{
		exists = reprap.GetPlatform().SysFileExists(present);
		missing = reprap.GetPlatform().SysFileExists(absent);
	});
	bool ok = exists && !missing && messages.empty() && internalErrors.empty();
	std::string why = "expected heap.g to exist and other.g not, without a timeout; messages: " + Join(messages);
	if (ok && setVariablePending && dsfRequestsAnswered.size() != 1)
	{
		ok = false;
		why = "the SetVariable request was not answered";
	}
	if (ok && !IsFree(Heap::heapLock))
	{
		ok = false;
		why = "Heap::heapLock is still held";
	}
	Report(std::string("C5: fileexists() of a string on the heap") + ((setVariablePending) ? " while a SetVariable of DSF with a string is pending" : ""), ok, why);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 5. Commands of text that call fileexists() or fileread() (C2/C3)

enum class BlockType : uint8_t { plain, ifTrue, ifFalseNoneTrue, ifFalseHadTrue, loop };		// src/GCodes/GCodeMachineState.h

// The GCodeBuffer as far as StringParser uses it here (src/GCodes/GCodeBuffer/GCodeBuffer.h)
class GCodeBuffer
{
public:
	void SendToSbc() noexcept { sendToSbc = true; }

	char buffer[256];
	bool sendToSbc = false;
};

// StringParser as far as the extracted functions use it (src/GCodes/GCodeBuffer/StringParser.h). The meta commands only record that they ran.
class StringParser
{
public:
	explicit StringParser(GCodeBuffer& p_gb) noexcept : gb(p_gb) { }

	void SetLine(const char *line) noexcept
	{
		std::snprintf(gb.buffer, sizeof(gb.buffer), "%s", line);
		gb.sendToSbc = false;
		commandStart = 0;
		gcodeLineEnd = commandEnd = (unsigned int)std::strlen(gb.buffer);
		readPointer = -1;
		processed.clear();
	}

	void SetCommand(unsigned int start, unsigned int end) noexcept { commandStart = start; commandEnd = end; }

	bool CallsFileFunction() const noexcept;
	static bool CallsFileFunction(const char *_ecv_array text, size_t length) noexcept;
	bool ProcessConditionalGCode(const StringRef& reply, BlockType skippedBlockType, bool doingFile) THROWS(GCodeException);

	std::string processed;
	unsigned int commandStart, commandEnd, gcodeLineEnd;

private:
	bool SendGlobalAssignmentToSbc(bool isSet) noexcept;

	void ProcessIfCommand() noexcept { processed = "if"; }
	void ProcessElseCommand(BlockType skippedBlockType) noexcept { processed = "else"; }
	void ProcessElifCommand(BlockType skippedBlockType) noexcept { processed = "elif"; }
	void ProcessWhileCommand() noexcept { processed = "while"; }
	void ProcessBreakCommand() noexcept { processed = "break"; }
	void ProcessContinueCommand() noexcept { processed = "continue"; }
	void ProcessVarOrGlobalCommand(bool isGlobal) noexcept { processed = (isGlobal) ? "global" : "var"; }
	void ProcessSetCommand() noexcept { processed = "set"; }
	void ProcessAbortCommand(const StringRef& reply) noexcept { processed = "abort"; }
	void ProcessEchoCommand(const StringRef& reply) noexcept { processed = "echo"; }

	GCodeBuffer& gb;
	int readPointer;
};

#include "StringParser.inc"

static GCodeBuffer gb;
static StringParser parser(gb);

static void TestScanner(const char *line, bool expected) noexcept
{
	failures.clear();
	parser.SetLine(line);
	const bool calls = parser.CallsFileFunction();
	Report(std::string("C2/C3 scanner: '") + line + "' " + ((expected) ? "calls" : "does not call") + " a file function",
			calls == expected, std::string("CallsFileFunction returned ") + ((calls) ? "true" : "false"));
}

// The current command is the part of the line from 'start' to 'end', as DecodeCommand finds it
static void TestScannerCommand(const char *line, unsigned int start, unsigned int end, bool expected) noexcept
{
	failures.clear();
	parser.SetLine(line);
	parser.SetCommand(start, end);
	const bool calls = parser.CallsFileFunction();
	Report(std::string("C2/C3 scanner: command '") + std::string(line + start, end - start) + "' of '" + line + "' " + ((expected) ? "calls" : "does not call") + " a file function",
			calls == expected, std::string("CallsFileFunction returned ") + ((calls) ? "true" : "false"));
}

// A meta command: 'sent' if StringParser passes the line to DSF instead of running it, else the command that it runs
static void TestMetaCommand(const char *line, bool doingFile, bool connected, const char *expected) noexcept
{
	failures.clear();
	reprap.usingSbc = true;
	reprap.GetSbcInterface().connected = connected;
	parser.SetLine(line);
	char replyBuffer[100] = { 0 };
	const bool done = parser.ProcessConditionalGCode(StringRef(replyBuffer, sizeof(replyBuffer)), BlockType::plain, doingFile);
	std::string result;
	if (gb.sendToSbc)
	{
		result = (!done && parser.commandStart == 0 && parser.commandEnd == parser.gcodeLineEnd) ? "sent" : "sent, but not as the whole line or marked as done";
	}
	else
	{
		result = (done) ? parser.processed : "not a meta command";
	}
	Report(std::string("C2/C3 meta command") + ((connected) ? "" : " without a connection to the SBC") + ": '" + line + "' " + ((std::strcmp(expected, "sent") == 0) ? "goes to DSF" : std::string("runs as ") + expected),
			result == expected, "result: " + result);
	reprap.GetSbcInterface().connected = true;
}

static void TestCommandsCallingFileFunctions() noexcept
{
	TestScanner("M558 P8 C\"io3.in\" H{fileexists(\"0:/sys/probe.g\") ? 5 : 2}", true);
	TestScanner("M308 S1 Y{fileread(\"0:/sys/sensor.csv\", 0, 1, ',')}", true);
	TestScanner("M950 H1 C{ fileexists ( \"0:/sys/h.g\" ) ? \"out1\" : \"out2\" }", true);
	TestScanner("M950 H1 C\"fileexists(out1)\"", false);							// in a quoted string
	TestScanner("M118 S\"say \"\"hi\"\" fileread(x)\"", false);						// doubled quotes inside a quoted string
	TestScanner("M118 S\"a\" ; fileexists(\"b\")", false);							// in a comment
	TestScanner("M118 S{var.myfileexists(1)}", false);								// part of another name
	TestScanner("M118 S{global.fileexists}", false);								// a variable named like the function
	TestScanner("M118 S{myfileread(1)}", false);
	TestScanner("M118 S{fileexists}", false);										// not called
	TestScanner("G1 X10 Y20", false);
	TestScanner("fileexists(\"a\")", true);											// at the start of the text
	TestScannerCommand("G1 X5 M118 S{fileexists(\"a\")}", 0, 6, false);				// the call is in the next command
	TestScannerCommand("G1 X5 M118 S{fileexists(\"a\")}", 6, 29, true);

	TestMetaCommand("set global.x = fileexists(\"0:/sys/a.g\")", false, true, "sent");
	TestMetaCommand("set   global.x = {fileread(\"a.csv\", 0, 1, ',')}", false, true, "sent");
	TestMetaCommand("global y = fileexists(\"0:/sys/a.g\")", false, true, "sent");
	TestMetaCommand("set var.x = fileexists(\"0:/sys/a.g\")", false, true, "set");		// local variables take no lock
	TestMetaCommand("var z = fileexists(\"0:/sys/a.g\")", false, true, "var");
	TestMetaCommand("echo fileexists(\"0:/sys/a.g\")", false, true, "echo");
	TestMetaCommand("set global.x = 1", false, true, "set");
	TestMetaCommand("global y = \"fileexists(a)\"", false, true, "global");
	TestMetaCommand("if fileexists(\"0:/sys/a.g\")", true, true, "if");
	TestMetaCommand("set global.x = fileexists(\"0:/sys/a.g\")", false, false, "set");
	TestMetaCommand("global y = fileexists(\"0:/sys/a.g\")", false, false, "global");
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// 6. Trigger actions (M581.1 U). An action runs in RRF alone, so M581.1 rejects one that calls fileexists() or fileread() anywhere on its
// line: in SBC mode DSF evaluates them

static inline bool isDigit(char c) noexcept
{
	return isdigit((int)c) != 0;
}

static uint32_t StrToU32(const char *s, const char **endptr) noexcept		// RRFLibraries src/General/SafeStrtod.h
{
	char *end;
	const uint32_t rslt = (uint32_t)std::strtoul(s, &end, 10);
	*endptr = end;
	return rslt;
}

// TriggerItem as far as the extracted functions use it (src/GCodes/TriggerItem.h)
class TriggerItem
{
public:
	static bool IsAllowedInAction(char letter, int number) noexcept;
	static bool IsAllowedAction(const char *_ecv_array s) noexcept;
};

#include "TriggerItem.inc"

static void TestAction(const char *action, bool expected) noexcept
{
	failures.clear();
	const bool allowed = TriggerItem::IsAllowedAction(action);
	Report(std::string("trigger action: '") + action + "' is " + ((expected) ? "allowed" : "rejected"),
			allowed == expected, std::string("IsAllowedAction returned ") + ((allowed) ? "true" : "false"));
}

static void TestTriggerActions() noexcept
{
	TestAction("set global.x = fileexists(\"0:/sys/a.g\")", false);
	TestAction("set global.x = {fileread(\"0:/sys/a.csv\", 0, 1, ',')}", false);
	TestAction("M118 S{fileexists(\"0:/sys/a.g\")}", false);
	TestAction("M118 S\"a\" M117 {fileexists(\"0:/sys/a.g\")}", false);		// a later command on the line
	TestAction("set global.x = global.y + 1", true);
	TestAction("set global.x = \"fileexists(a)\"", true);					// in a quoted string
	TestAction("M42 P1 S1 ; fileexists(\"a\")", true);					// in a comment
	TestAction("echo global.x", true);
	TestAction("M98 P\"a.g\"", false);									// runs a macro, rejected before as well
	TestAction("echo >\"a.txt\" 1", false);								// writes a file, rejected before as well
}

// ---------------------------------------------------------------------------------------------------------------------------------------

int main()
{
	TestWriteLockCount();

	for (MainHolds holds : { MainHolds::nothing, MainHolds::readLock, MainHolds::writeLock })
	{
		for (bool dsfWrites : { false, true })
		{
			TestWaitUnderLock(holds, dsfWrites);
		}
	}

	for (Announcement kind : { Announcement::restartV1, Announcement::restartV0, Announcement::reconnect })
	{
		for (bool collectionRunning : { true, false })
		{
			for (bool newFirmware : { false, true })
			{
				TestAnnouncement(kind, collectionRunning, newFirmware);
			}
		}
	}

	TestFileExistsOfHeapString(false);
	TestFileExistsOfHeapString(true);

	TestCommandsCallingFileFunctions();

	TestTriggerActions();

	std::printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
