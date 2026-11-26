// Host test of the lifetime of object model objects while another task uses the result of an expression.
//
// The object model holds the lock of a table only while it fetches a value (ObjectModel::GetObjectValueUsingTableNumber in
// src/ObjectModel/ObjectModel.cpp), so an expression that ends at an object model array returns a bare pointer to the object that owns
// the array. The SBC task (SbcInterface.cpp, SetVariable and EvaluateExpression) and, without an SBC, the network task
// (FileInfoParser::ProcessCustomInfo for rr_fileinfo) use that pointer after every lock is gone, while the main task deletes object model
// objects, e.g. the kinematics in M669 (Move::SetKinematics) and a tool in M563 (Tool::DeleteTool). ObjectModel::lifetimeLock closes that
// gap: those readers hold it for reading until they have finished with the result, and Main holds it for writing while it deletes.
//
// The test compiles the real text of these readers and deleting commands and the real ReadWriteLock, ReadLocker, WriteLocker and
// WriteLockedPointer of RRFLibraries, which the Makefile extracts at build time. It runs the tasks as threads on one simulated processor:
// exactly one task runs at a time, a task runs until it blocks unless a task with a higher priority becomes ready, and a mutex passes on
// the priority of a task that waits for it (FreeRTOS priority inheritance, given back only when the holder holds no other mutex). The
// calls that the real code makes into stand-ins are the points where another task can get the processor: there the test lets the SBC
// task's request arrive, or gives the time slice to the other task of the same priority. It tries every such point in turn, in SBC mode
// also with the CAN receiver allocating for a board announcement while the SBC task holds the result.
//
// Each object model object here records in a volatile field whether its destructor has run, and the readers check that field before they
// call into the object. The plain build keeps freed memory, so the check reports the use. The AddressSanitizer build frees it, so the
// check itself reads freed memory and ASan stops the test with a heap-use-after-free report.

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

// eCv annotations (RRFLibraries ecv_duet3d.h)
#define _ecv_array
#define _ecv_null
#define _ecv_from
#define null
#define _ecv_not_null(x)	(x)
#define not_null(x)			(x)
#define pre(...)
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
	static void TimeSlice() noexcept;
	static void Block() noexcept;
	static void MakeReady(TaskBase *t) noexcept;
}

// Stand-in for the TaskBase of RRFLibraries RTOSIface.h, as far as ReadWriteLock uses it
class TaskBase
{
public:
	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;
	enum class State { ready, blocked, finished };

	TaskBase(const char *p_name, unsigned int p_priority) noexcept : name(p_name), basePriority(p_priority), priority(p_priority) { }

	// xTaskNotifyGiveIndexed
	void Give(uint32_t index) noexcept
	{
		++notifyCount;
		if (waitingForNotification)
		{
			waitingForNotification = false;
			Cpu::MakeReady(this);
		}
	}

	// ulTaskNotifyTakeIndexed(index, pdTRUE, timeout): wait for a notification and clear the count. The lock code waits without a timeout.
	static bool TakeIndexed(uint32_t index, uint32_t timeout = TimeoutUnlimited) noexcept
	{
		TaskBase *const me = Cpu::running;
		if (me->notifyCount == 0)
		{
			me->waitingForNotification = true;
			Cpu::Block();
		}
		me->notifyCount = 0;
		return true;
	}

	static TaskBase *GetCallerTaskHandle() noexcept { return Cpu::running; }

	const char *const name;
	const unsigned int basePriority;
	unsigned int priority;						// the base priority, or one that it inherited through a mutex
	unsigned int writeLocksHeld = 0;			// RTOSIface.h: the write locks on ReadWriteLock objects that this task holds, which the lock code counts
	unsigned int mutexesHeld = 0;
	State state = State::ready;
	bool waitingForNotification = false;
	uint32_t notifyCount = 0;
	unsigned int hooksPassed = 0;
	std::function<void()> body;
	std::thread thread;
};

namespace NotifyIndices
{
	constexpr uint32_t ReadWriteLocker = 0;		// RRFLibraries RTOSNotifyIndices.h
}

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
		if (me != nullptr && me->state != TaskBase::State::finished)
		{
			cv.wait(lock, [me] { return running == me; });
		}
	}

	// The highest priority ready task, taking the tasks after 'after' first among equals (round robin)
	static TaskBase *Pick(const TaskBase *after, bool includeAfter) noexcept
	{
		size_t start = 0;
		for (size_t i = 0; i < tasks.size(); ++i)
		{
			if (tasks[i] == after) { start = i + 1; }
		}
		TaskBase *best = nullptr;
		for (size_t n = 0; n < tasks.size(); ++n)
		{
			TaskBase *const t = tasks[(start + n) % tasks.size()];
			if (t->state == TaskBase::State::ready && (includeAfter || t != after) && (best == nullptr || t->priority > best->priority))
			{
				best = t;
			}
		}
		return best;
	}

	// The running task has blocked or finished: run the next one
	static void Reschedule() noexcept
	{
		TaskBase *const me = running;
		TaskBase *const next = Pick(me, false);
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
		TaskBase *const next = Pick(me, false);
		if (next != nullptr && next->priority > me->priority)
		{
			SwitchTo(next);
		}
	}

	// The tick ends the time slice: a ready task of the same priority gets the processor
	static void TimeSlice() noexcept
	{
		if (suspended != 0)
		{
			return;
		}
		TaskBase *const me = running;
		TaskBase *const next = Pick(me, false);
		if (next != nullptr && next->priority >= me->priority)
		{
			SwitchTo(next);
		}
	}

	static void Block() noexcept
	{
		running->state = TaskBase::State::blocked;
		Reschedule();
	}

	static void MakeReady(TaskBase *t) noexcept
	{
		if (t->state == TaskBase::State::blocked)
		{
			t->state = TaskBase::State::ready;
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

// A recursive FreeRTOS mutex (RRFLibraries Mutex) with priority inheritance
class Mutex
{
public:
	void Take() noexcept
	{
		TaskBase *const me = Cpu::running;
		while (owner != nullptr && owner != me)
		{
			if (owner->priority < me->priority)
			{
				owner->priority = me->priority;				// xTaskPriorityInherit
			}
			waiters.push_back(me);
			Cpu::Block();
		}
		if (count++ == 0)
		{
			owner = me;
			++me->mutexesHeld;
		}
	}

	// The SBC task takes the mutex of its channel with a timeout of SbcYieldTimeout. The test never makes it wait, so this does not block.
	bool Take(uint32_t timeout) noexcept
	{
		if (owner != nullptr && owner != Cpu::running)
		{
			return false;
		}
		Take();
		return true;
	}

	void Release() noexcept
	{
		if (--count != 0)
		{
			return;
		}
		TaskBase *const me = Cpu::running;
		owner = nullptr;
		if (--me->mutexesHeld == 0)
		{
			me->priority = me->basePriority;				// FreeRTOS tasks.c xTaskPriorityDisinherit: only when the task holds no other mutex
		}
		if (!waiters.empty())
		{
			size_t best = 0;
			for (size_t i = 1; i < waiters.size(); ++i)
			{
				if (waiters[i]->priority > waiters[best]->priority) { best = i; }
			}
			TaskBase *const w = waiters[best];
			waiters.erase(waiters.begin() + best);
			Cpu::MakeReady(w);
		}
		else
		{
			Cpu::MaybePreempt();
		}
	}

private:
	TaskBase *owner = nullptr;
	unsigned int count = 0;
	std::vector<TaskBase *> waiters;
};

class MutexLocker
{
public:
	explicit MutexLocker(Mutex& m, uint32_t timeout = TaskBase::TimeoutUnlimited) noexcept : mutex(m)
	{
		if (timeout == TaskBase::TimeoutUnlimited)
		{
			m.Take();
			acquired = true;
		}
		else
		{
			acquired = m.Take(timeout);
		}
	}
	~MutexLocker() { if (acquired) { mutex.Release(); } }
	bool IsAcquired() const noexcept { return acquired; }

private:
	Mutex& mutex;
	bool acquired;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// Points where another task can get the processor. Each task counts the points it passes; the run that the test sets up does its action
// at one of them.

enum class HookAction { none, sbcRequestArrives, timeSlice };

static TaskBase *hookTask = nullptr;
static unsigned int hookIndex = 0;
static HookAction hookAction = HookAction::none;
static TaskBase *sbcTask = nullptr;
static TaskBase *canReceiverTask = nullptr;
static bool announcementPending = false;			// an expansion board announces itself as soon as the SBC task has read move.kinematics
static std::string actionPoint;						// where the action happened in this run

static void Hook(const char *point) noexcept
{
	TaskBase *const me = Cpu::running;
	if (announcementPending && me == sbcTask && std::strcmp(point, "parse: after reading move.kinematics") == 0)
	{
		announcementPending = false;
		Cpu::MakeReady(canReceiverTask);				// the CAN receiver has a higher priority than the SBC task
	}
	if (me != nullptr && me == hookTask)
	{
		if (me->hooksPassed++ == hookIndex && hookAction != HookAction::none)
		{
			actionPoint = point;
			const HookAction action = hookAction;
			hookAction = HookAction::none;
			if (action == HookAction::sbcRequestArrives)
			{
				Cpu::MakeReady(sbcTask);					// the SBC task wakes up for the next transfer and has a higher priority
			}
			else
			{
				Cpu::TimeSlice();
			}
		}
	}
}

// newlib's malloc and free with RRF's __malloc_lock (GetMallocMutex in src/Platform/Tasks.cpp), which takes the mutex only while the
// scheduler runs, so not inside a task critical section
static Mutex mallocMutex;
static std::vector<void *> keptMemory;				// freed memory that the plain build keeps, so that a use after free reads defined values

static void *Malloc(size_t sz, const char *point) noexcept
{
	const bool lock = (Cpu::running != nullptr && Cpu::suspended == 0);		// the test sets up and cleans up outside the tasks
	if (lock) { mallocMutex.Take(); }
	Hook(point);
	void *const p = std::malloc(sz);
	if (lock) { mallocMutex.Release(); }
	return p;
}

static void ReleaseMemory(void *p) noexcept
{
#if defined(__SANITIZE_ADDRESS__)
	std::free(p);
#else
	keptMemory.push_back(p);
#endif
}

static void Free(void *p, const char *point) noexcept
{
	const bool lock = (Cpu::running != nullptr && Cpu::suspended == 0);
	if (lock) { mallocMutex.Take(); }
	Hook(point);
	ReleaseMemory(p);
	if (lock) { mallocMutex.Release(); }
}

// RRFLibraries FreelistManager.h. A freelist hands out and takes back items in a task critical section, and takes a new item from
// CoreAllocPermanent when it is empty. The stand-in calls malloc directly, without the malloc mutex, so the lock records never wait here
// (README, "Not covered").
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

// The real ReadWriteLock, ReadLocker, WriteLocker and WriteLockedPointer
struct LockRecord;
#include "RTOSIfaceClasses.inc"
#include "RTOSIface.inc"

// ---------------------------------------------------------------------------------------------------------------------------------------
// The object model, as far as the readers use it

class ObjectModel;
class ExpressionValue;

class ObjectExplorationContext
{
public:
	void AddIndex(int32_t index) noexcept { indices.push_back(index); }
	void RemoveIndex() noexcept { indices.pop_back(); }
	int32_t GetLastIndex() const noexcept { return indices.back(); }

private:
	std::vector<int32_t> indices;
};

// src/ObjectModel/ObjectModel.h
struct ObjectModelArrayTableEntry
{
	ReadWriteLock *null lockPointer;
	size_t (*GetNumElements)(const ObjectModel *_ecv_from, const ObjectExplorationContext&) noexcept;
	ExpressionValue (*GetElement)(const ObjectModel *_ecv_from, ObjectExplorationContext&) noexcept;
};

enum class TypeCode : uint32_t { None, Float, HeapArray, ObjectModelArray };

struct ArrayHandle { };								// the expressions in this test give no heap arrays

class ExpressionValue
{
public:
	ExpressionValue() noexcept : type((uint32_t)TypeCode::None), param(0), omVal(nullptr) { }
	explicit ExpressionValue(float f) noexcept : type((uint32_t)TypeCode::Float), param(0), fVal(f) { }
	ExpressionValue(const ObjectModel *om, uint8_t arrayIndex) noexcept : type((uint32_t)TypeCode::ObjectModelArray), param(arrayIndex), omVal(om) { }

	TypeCode GetType() const noexcept { return (TypeCode)type; }

	uint32_t type;
	uint32_t param;
	union
	{
		float fVal;
		const ObjectModel *omVal;
	};
	ArrayHandle ahVal;
};

// src/Config/Configuration.h, for the MB6HC (SAME70)
constexpr size_t OUTPUT_BUFFER_SIZE = 256;
constexpr size_t OUTPUT_BUFFER_COUNT = 40;
constexpr size_t RESERVED_OUTPUT_BUFFERS = 4;

// src/Platform/OutputMemory.h. The stand-in holds text of any length in one buffer, so it never overflows, and all buffers are free.
class OutputBuffer
{
public:
	// src/Platform/OutputMemory.cpp: takes a free buffer without waiting
	static bool Allocate(OutputBuffer *_ecv_null &buf) noexcept { buf = new OutputBuffer; return true; }
	static void ReleaseAll(OutputBuffer *_ecv_null volatile &buf) noexcept { delete buf; buf = nullptr; }
	static unsigned int GetFreeBuffers() noexcept { return OUTPUT_BUFFER_COUNT; }
	void cat(const char *s) noexcept { text += s; }
	size_t Length() const noexcept { return text.size(); }
	bool HadOverflow() const noexcept { return false; }
	std::string text;
};

class ObjectModel
{
public:
	ObjectModel() noexcept : state(Intact) { }
	virtual ~ObjectModel() { state = Destroyed; }

	static ReadWriteLock lifetimeLock;				// src/ObjectModel/ObjectModel.h

	const ObjectModelArrayTableEntry *_ecv_null FindObjectModelArrayEntry(unsigned int index) const noexcept { return GetObjectModelArrayEntry(index); }

	// The test's check before a reader calls into the object. In the AddressSanitizer build this read is the use after free that ASan reports.
	bool IsIntact(const char *use) const noexcept
	{
		if (state != Intact)
		{
			Fail(std::string(use) + " uses an object that Main has deleted");
			return false;
		}
		return true;
	}

	// ObjectModel::ReportItemAsJsonFull and ReportObjectModelArrayAsJson in src/ObjectModel/ObjectModel.cpp for a 1D array of floats
	void ReportItemAsJsonFull(OutputBuffer *buf, ObjectExplorationContext& context, const void *_ecv_null classDescriptor, const ExpressionValue& val,
								const char *_ecv_array filter) const THROWS(GCodeException)
	{
		if (!IsIntact("the report of the object model array"))
		{
			return;
		}
		const ObjectModelArrayTableEntry *const entry = _ecv_not_null(val.omVal->FindObjectModelArrayEntry(val.param & 0xFF));
		ReadLocker lock(entry->lockPointer);
		buf->cat("[");
		const size_t count = entry->GetNumElements(this, context);
		for (size_t i = 0; i < count; ++i)
		{
			context.AddIndex(i);
			buf->cat(((i == 0) ? "" : ","));
			buf->cat(std::to_string((int)entry->GetElement(this, context).fVal).c_str());
			context.RemoveIndex();
		}
		buf->cat("]");
	}

protected:
	virtual const ObjectModelArrayTableEntry *_ecv_null GetObjectModelArrayEntry(unsigned int index) const noexcept = 0;

private:
	static constexpr uint32_t Intact = 0x494E5443, Destroyed = 0xDEADDEAD;
	volatile uint32_t state;						// volatile so that the store in the destructor is not optimised away
};

ReadWriteLock ObjectModel::lifetimeLock;

// Variable.cpp: Variable::CopyObjectModelArray, which Variable::MakeStorable calls for an object model array
static std::vector<float> CopyObjectModelArray(const ExpressionValue& ev, const char *use)
{
	std::vector<float> copy;
	if (!ev.omVal->IsIntact(use))
	{
		return copy;
	}
	const ObjectModelArrayTableEntry *const entry = ev.omVal->FindObjectModelArrayEntry(ev.param & 0xFF);
	ReadLocker lock(entry->lockPointer);
	ObjectExplorationContext context;
	const size_t numElements = entry->GetNumElements(ev.omVal, context);
	for (size_t i = 0; i < numElements; ++i)
	{
		context.AddIndex(i);
		copy.push_back(entry->GetElement(ev.omVal, context).fVal);
		context.RemoveIndex();
	}
	return copy;
}

// The object model arrays of the test objects: three floats, as move.kinematics.forwardMatrix and tools[].offsets are arrays of numbers
class ThreeFloats : public ObjectModel
{
public:
	explicit ThreeFloats(float base) noexcept { for (size_t i = 0; i < 3; ++i) { values[i] = base + i; } }

protected:
	const ObjectModelArrayTableEntry *_ecv_null GetObjectModelArrayEntry(unsigned int index) const noexcept override { return &arrayEntry; }

private:
	static size_t NumElements(const ObjectModel *, const ObjectExplorationContext&) noexcept { return 3; }
	static ExpressionValue Element(const ObjectModel *self, ObjectExplorationContext& context) noexcept
	{
		return ExpressionValue(static_cast<const ThreeFloats *>(self)->values[context.GetLastIndex()]);
	}

	static constexpr ObjectModelArrayTableEntry arrayEntry = { nullptr, NumElements, Element };
	float values[3];
};

static std::string Json(float base) noexcept
{
	return "[" + std::to_string((int)base) + "," + std::to_string((int)base + 1) + "," + std::to_string((int)base + 2) + "]";
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Variables and the expression parser

class GCodeBuffer;

class GCodeException
{
public:
	explicit GCodeException(const char *msg) noexcept : message(msg) { }
	void GetMessage(const class StringRef& reply, const GCodeBuffer *_ecv_null gb) const noexcept;
	void DebugPrint() const noexcept { }

private:
	const char *message;
};

class StringRef
{
public:
	StringRef(char *pp, size_t pl) noexcept : p(pp), len(pl) { }
	const char *c_str() const noexcept { return p; }
	void copy(const char *s) const noexcept { std::snprintf(p, len, "%s", s); }
	int printf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = std::vsnprintf(p, len, fmt, vargs);
		va_end(vargs);
		return ret;
	}

private:
	char *p;
	size_t len;
};

void GCodeException::GetMessage(const StringRef& reply, const GCodeBuffer *_ecv_null gb) const noexcept { reply.copy(message); }

template<size_t N> class String
{
public:
	String() noexcept { buf[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(buf, N + 1); }
	const char *c_str() const noexcept { return buf; }
	size_t strlen() const noexcept { return std::strlen(buf); }
	void copy(const char *s) noexcept { std::snprintf(buf, N + 1, "%s", s); }
	char& operator[](size_t index) noexcept { return buf[index]; }
	int printf(const char *fmt, ...) noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = std::vsnprintf(buf, N + 1, fmt, vargs);
		va_end(vargs);
		return ret;
	}

private:
	char buf[N + 1];
};

constexpr size_t StringLength100 = 100;
constexpr size_t MaxVariableNameLength = 120;			// src/Config/Configuration.h
constexpr size_t MaxGCodeStringLength = 256;			// src/Config/Configuration.h
constexpr size_t MaxExpressionArrayIndices = 5;			// src/Config/Configuration.h
constexpr size_t MaxSbcExpressionLength = 256;			// src/SBC/SbcMessageFormats.h

class Variable
{
public:
	void Assign(ExpressionValue& ev) THROWS(GCodeException) { value = CopyObjectModelArray(ev, "the assignment"); }
	void AssignIndexed(const ExpressionValue& ev, size_t numIndices, const uint32_t *_ecv_array indices) THROWS(GCodeException) { }

	std::string name;
	std::vector<float> value;
};

// src/ObjectModel/Variable.h and Variable.cpp
class VariableSet
{
public:
	Variable *_ecv_null Lookup(const char *_ecv_array str, bool wantParameter) noexcept { return Lookup(str, std::strlen(str), wantParameter); }
	Variable *_ecv_null Lookup(const char *_ecv_array str, size_t length, bool wantParameter) noexcept
	{
		Hook("VariableSet::Lookup");
		for (LinkedVariable *lv = root; lv != nullptr; lv = lv->next)
		{
			if (lv->v.name == std::string(str, length)) { return &lv->v; }
		}
		return nullptr;
	}

	Variable *InsertNew(const char *_ecv_array str, ExpressionValue pVal, int16_t pScope) THROWS(GCodeException) { return InsertNew(str, std::strlen(str), pVal, pScope); }

	// Variable.cpp: new LinkedVariable, then its Variable constructor stores the value with MakeStorable, which copies an object model
	// array to the heap (CopyObjectModelArray). LinkedVariable has a freelist, which calls malloc when it is empty (FreelistManager.h).
	Variable *InsertNew(const char *_ecv_array str, size_t strLen, ExpressionValue pVal, int16_t pScope) THROWS(GCodeException)
	{
		LinkedVariable *const toInsert = new LinkedVariable;
		Hook("VariableSet::InsertNew, before storing the value");
		toInsert->v.name.assign(str, strLen);
		toInsert->v.value = CopyObjectModelArray(pVal, "VariableSet::InsertNew");
		toInsert->next = root;
		root = toInsert;
		return &toInsert->v;
	}

	~VariableSet()
	{
		while (root != nullptr)
		{
			LinkedVariable *const lv = root;
			root = lv->next;
			delete lv;
		}
	}

private:
	struct LinkedVariable
	{
		void *operator new(size_t sz) noexcept { return Malloc(sz, "malloc for the new variable"); }
		void operator delete(void *p) noexcept { std::free(p); }

		LinkedVariable *next = nullptr;
		Variable v;
	};

	LinkedVariable *root = nullptr;
};

// src/ObjectModel/GlobalVariables.h and .cpp
class GlobalVariables
{
public:
	WriteLockedPointer<VariableSet> GetForWriting() noexcept { return WriteLockedPointer<VariableSet>(lock, &vars); }
	VariableSet& GetVariables() noexcept { return vars; }

private:
	VariableSet vars;
	ReadWriteLock lock;
};

class Kinematics;

class Move
{
public:
	bool SetKinematics(const char *_ecv_array _ecv_null name, int legacyType) noexcept;

	Kinematics *_ecv_from kinematics = nullptr;
};

class Tool;

// The expression parser as far as it evaluates these two expressions. Like ObjectModel::GetObjectValueUsingTableNumber and
// ExpressionParser::ApplyObjectModelArrayIndex it holds a table lock only while it fetches the value, and returns the object model array
// as a pointer to the object that owns it.
class ExpressionParser
{
public:
	ExpressionParser(const GCodeBuffer *_ecv_null p_gb, const char *_ecv_array text, const char *_ecv_array textLimit = nullptr, int column = -1) noexcept
		: expression(text, (textLimit != nullptr) ? (size_t)(textLimit - text) : std::strlen(text)), endptr(text) { }

	ExpressionValue Parse(bool evaluate = true) THROWS(GCodeException);
	uint32_t ParseUnsigned() THROWS(GCodeException) { throw GCodeException("no array indices in this test"); }
	const char *_ecv_array GetEndptr() const noexcept { return endptr; }
	void CheckForExtraCharacters() THROWS(GCodeException) { }

private:
	std::string expression;
	const char *endptr;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The tasks' objects: the kinematics, the tools, RepRap

static Move move;
static unsigned int kinematicsDeleted = 0;
static bool parseFoundTool = false;					// the parse of tools[0].offsets found tool 0

class Kinematics : public ThreeFloats
{
public:
	explicit Kinematics(float base) noexcept : ThreeFloats(base) { }
	static Kinematics *_ecv_null Create(const char *_ecv_array _ecv_null name, int legacyType) noexcept;

	// Kinematics objects come from malloc
	void *operator new(size_t sz) noexcept { return Malloc(sz, "malloc for the new kinematics"); }
	void operator delete(void *p) noexcept { ++kinematicsDeleted; Free(p, "free of the old kinematics"); }

	~Kinematics() override { Hook("destructor of the old kinematics"); }
};

static constexpr float OldMatrix = 10.0, NewMatrix = 20.0, ToolOffsets = 30.0;
static Kinematics *createdKinematics = nullptr;

Kinematics *Kinematics::Create(const char *_ecv_array _ecv_null name, int legacyType) noexcept
{
	Hook("Kinematics::Create");
	createdKinematics = new Kinematics(NewMatrix);
	return createdKinematics;
}

class Tool : public ThreeFloats
{
public:
	explicit Tool(int n) noexcept : ThreeFloats(ToolOffsets), myNumber(n) { }

	static void DeleteTool(int toolNumber) noexcept;					// the real text from Tool.cpp
	static void Delete(Tool *t) noexcept { delete t; }					// Tool.h
	static Tool *_ecv_null Find(int n) noexcept
	{
		for (Tool *t = toolList; t != nullptr; t = t->next) { if (t->myNumber == n) { return t; } }
		return nullptr;
	}

	int Number() const noexcept { return myNumber; }
	Tool *_ecv_null Next() const noexcept { return next; }
	size_t HeaterCount() const noexcept { return 1; }
	int GetHeater(size_t) const noexcept { return 1; }
	void UpdateExtruderAndHeaterCount(uint16_t& numExtruders, uint16_t& numHeaters) const noexcept { ++numExtruders; ++numHeaters; }

	// Tool has a freelist (DECLARE_FREELIST_NEW_DELETE in Tool.h), which takes an item back in a task critical section without malloc
	void *operator new(size_t sz) noexcept { return std::malloc(sz); }
	void operator delete(void *p) noexcept { ++toolsDeleted; ReleaseMemory(p); }

	static ReadWriteLock toolListLock;
	static Tool *_ecv_null toolList;
	static uint16_t activeExtruders, activeToolHeaters, numToolsToReport;
	static unsigned int toolsDeleted;

	Tool *_ecv_null next = nullptr;

private:
	int myNumber;
};

ReadWriteLock Tool::toolListLock;
Tool *_ecv_null Tool::toolList = nullptr;
uint16_t Tool::activeExtruders = 0, Tool::activeToolHeaters = 0, Tool::numToolsToReport = 0;
unsigned int Tool::toolsDeleted = 0;

ExpressionValue ExpressionParser::Parse(bool evaluate) THROWS(GCodeException)
{
	const size_t first = expression.find_first_not_of(' ');
	const std::string e = (first == std::string::npos) ? std::string() : expression.substr(first);
	endptr += expression.size();
	if (e == "move.kinematics.forwardMatrix")
	{
		// move.kinematics is OBJECT_MODEL_FUNC(self->kinematics) in Move.cpp, without a table lock
		Hook("parse: before reading move.kinematics");
		const Kinematics *const k = move.kinematics;
		Hook("parse: after reading move.kinematics");
		return ExpressionValue(k, 0);
	}
	if (e == "tools[0].offsets")
	{
		// tools is array 4 of RepRap.cpp, locked by Tool::toolListLock, and its element is Tool::GetLockedTool(index).Ptr()
		const Tool *_ecv_null tool;
		{
			Hook("parse: before taking toolListLock");
			ReadLocker lock(Tool::toolListLock);
			Hook("parse: holding toolListLock");
			tool = Tool::Find(0);
		}
		parseFoundTool = (tool != nullptr);
		Hook("parse: after fetching tools[0]");
		if (tool == nullptr)
		{
			throw GCodeException("unknown value 'tools^.offsets'");
		}
		return ExpressionValue(tool, 0);
	}
	throw GCodeException("unexpected expression");
}

class Heat
{
public:
	void SwitchOff(int heater) noexcept { Hook("Heat::SwitchOff"); }
};

class GCodeBuffer
{
public:
	bool IsRecordPending() const noexcept { return false; }
	VariableSet& GetVariables() noexcept { return vars; }

	Mutex mutex;

private:
	VariableSet vars;
};

typedef uint8_t GCodeChannel_t;
struct GCodeChannel
{
	bool IsValid() const noexcept { return valid; }
	bool valid;
};

class GCodes
{
public:
	GCodeBuffer *_ecv_null GetGCodeBuffer(GCodeChannel channel) noexcept { return &sbcGb; }

	GCodeBuffer mainGb, sbcGb;			// the channel on which Main runs the command, and the one of the SBC request
};

enum class Module { PrintMonitor };

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return gcodes; }
	Heat& GetHeat() noexcept { return heat; }
	WriteLockedPointer<VariableSet> GetGlobalVariablesForWriting() noexcept { return globalVariables.GetForWriting(); }
	void GlobalUpdated() noexcept { }
	void MoveUpdated() noexcept { Hook("RepRap::MoveUpdated"); }
	void ToolsUpdated() noexcept { Hook("RepRap::ToolsUpdated"); }
	bool Debug(Module m) const noexcept { return false; }

	void Reset() noexcept { gcodes.~GCodes(); new (&gcodes) GCodes; globalVariables.~GlobalVariables(); new (&globalVariables) GlobalVariables; }

	GCodes gcodes;
	Heat heat;
	GlobalVariables globalVariables;
};

static RepRap reprap;

#define REPORT_INTERNAL_ERROR	Fail("internal error reported")

// ---------------------------------------------------------------------------------------------------------------------------------------
// The SBC task: the cases of SbcInterface::ExchangeData that evaluate an expression

enum class SbcRequest : uint16_t { EvaluateExpression, SetVariable };

// src/SBC/SbcMessageFormats.h
constexpr size_t SbcTransferBufferSize = 8192;

struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

enum class DataType : uint8_t;

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

static uint16_t ChannelEpochOf(uint16_t id) noexcept { return 0; }
static uint16_t ChannelEpochTag(uint16_t epoch) noexcept { return 0; }
static bool StringStartsWith(const char *_ecv_array string, const char *_ecv_array starting) noexcept { return std::strncmp(string, starting, std::strlen(starting)) == 0; }
static uint32_t millis() noexcept { return 0; }

// src/SBC/SbcInterface.cpp
constexpr uint32_t SbcYieldTimeout = 10;
constexpr size_t MinimumBuffersForSbcJson = RESERVED_OUTPUT_BUFFERS + (SbcTransferBufferSize + OUTPUT_BUFFER_SIZE - 1)/OUTPUT_BUFFER_SIZE;
constexpr size_t MaxSbcAnswerDataLength = SbcTransferBufferSize - 2 * sizeof(PacketHeader);
constexpr const char *_ecv_array ValueTooLongForSbc = "value too long to send to the SBC";

// What DSF sent and what RRF sent back
struct SbcExchange
{
	std::string expression;
	std::string varName;
	bool createVariable = false;
	std::string result;
	bool answered = false;
};

static SbcExchange sbcExchange;

class DataTransfer
{
public:
	GCodeChannel ReadEvaluateExpression(size_t packetLength, const StringRef& expression) noexcept
	{
		expression.copy(sbcExchange.expression.c_str());
		return GCodeChannel{true};
	}
	GCodeChannel ReadSetVariable(bool& createVariable, const StringRef& varName, const StringRef& expression) noexcept
	{
		createVariable = sbcExchange.createVariable;
		varName.copy(sbcExchange.varName.c_str());
		expression.copy(sbcExchange.expression.c_str());
		return GCodeChannel{true};
	}
	bool WriteEvaluationResult(GCodeChannel channel, const char *expression, OutputBuffer *json, uint16_t tag) noexcept { return Answer(json); }
	bool WriteEvaluationResult(GCodeChannel channel, const char *expression, const ExpressionValue& value, uint16_t tag) noexcept { return Answer("plain value"); }
	bool WriteEvaluationError(GCodeChannel channel, const char *expression, const char *errorMessage, uint16_t tag) noexcept { return Answer(std::string("error: ") + errorMessage); }
	bool WriteSetVariableResult(GCodeChannel channel, const char *varName, OutputBuffer *json, uint16_t tag) noexcept { return Answer(json); }
	bool WriteSetVariableResult(GCodeChannel channel, const char *varName, const ExpressionValue& value, uint16_t tag) noexcept { return Answer("plain value"); }
	bool WriteSetVariableError(GCodeChannel channel, const char *varName, const char *errorMessage, uint16_t tag) noexcept { return Answer(std::string("error: ") + errorMessage); }

	// src/SBC/DataTransfer.h: the transfer of this test has room for every answer
	bool CanWriteSetVariableResult(const char *varName, const ExpressionValue& value) const noexcept { return true; }
	bool CanWriteSetVariableResult(const char *varName, const OutputBuffer *json) const noexcept { return true; }

private:
	static bool Answer(OutputBuffer *json) noexcept { const bool ok = Answer(json->text); delete json; return ok; }
	static bool Answer(const std::string& s) noexcept { sbcExchange.result = s; sbcExchange.answered = true; return true; }
};

class SbcInterface
{
public:
	bool HandlePacket(const PacketHeader *packet) noexcept;			// the two cases of ExchangeData, with its packetAcknowledged

private:
	bool IsDroppedByEpoch(GCodeBuffer&, const PacketHeader&) noexcept { return false; }
	bool MakeJsonAnswer(OutputBuffer *_ecv_null &json, const ExpressionValue& val, size_t otherDataLength) THROWS(GCodeException);	// the real text
	void ReportHeapArrayAsJson(OutputBuffer *buf, ObjectExplorationContext& context, const void *_ecv_null classDescriptor, ArrayHandle ah,
								const char *_ecv_array filter) const noexcept { }

	DataTransfer transfer;
	uint32_t burstModeStartTime = 0;
};

bool SbcInterface::HandlePacket(const PacketHeader *packet) noexcept
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

#include "MakeJsonAnswer.inc"

// ---------------------------------------------------------------------------------------------------------------------------------------
// The network task: file info for rr_fileinfo, which stores ;CustomInfo values in a set of variables

static bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool isAlnum(char c) noexcept { return isAlpha(c) || (c >= '0' && c <= '9'); }

class FileInfoParser
{
public:
	void ProcessCustomInfo(const char *_ecv_array k, const char *_ecv_array p, const char *_ecv_array lineEnd, int param) noexcept;

	GlobalVariables *_ecv_null vars = nullptr;
};

// The real text of the readers and of the deleting commands
#include "ReplaceObject.inc"
#include "SetKinematics.inc"
#include "DeleteTool.inc"
#include "ProcessCustomInfo.inc"

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

static void ResetState() noexcept
{
	failures.clear();
	actionPoint.clear();
	sbcExchange = SbcExchange();
	reprap.Reset();
	kinematicsDeleted = 0;
	Tool::toolsDeleted = 0;
	parseFoundTool = false;
	rtosAsserts = 0;
}

static void FreeKeptMemory() noexcept
{
	for (void *p : keptMemory) { std::free(p); }
	keptMemory.clear();
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

// SBC mode. Main runs M669 K5 on its channel, holding the mutex of that channel as GCodes::SpinGCodeBuffer does, and the new kinematics
// replaces the old one in Move::SetKinematics. The SBC task (priority 2, Main has 1) gets a request at one of Main's points and runs
// until it has to wait: 'global k = move.kinematics.forwardMatrix' (SetVariable) or 'echo move.kinematics.forwardMatrix'
// (EvaluateExpression). Returns the number of points that Main passes.
// With 'announcement', an expansion board announces itself right after the SBC task has read move.kinematics. The CAN receiver (priority 6)
// allocates the board's data in ExpansionManager::ProcessAnnouncement, and if Main holds the malloc mutex, Main inherits priority 6 and
// keeps it until it releases the mutex of its channel, so it runs before the SBC task again.
static unsigned int RunKinematicsChange(SbcRequest request, bool announcement, unsigned int arrivalPoint, bool countOnly) noexcept
{
	ResetState();
	move.kinematics = new Kinematics(OldMatrix);
	Kinematics *const oldKinematics = move.kinematics;
	if (request == SbcRequest::SetVariable)
	{
		sbcExchange.createVariable = true;
		sbcExchange.varName = "global.k";
	}
	sbcExchange.expression = "move.kinematics.forwardMatrix";

	TaskBase mainTask("Main", 1), sbc("SBC", 2), canReceiver("CanReceiver", 6);
	sbcTask = &sbc;
	sbc.state = TaskBase::State::blocked;		// waiting for the next transfer
	canReceiverTask = &canReceiver;
	canReceiver.state = TaskBase::State::blocked;	// waiting for a CAN message
	announcementPending = announcement;
	hookTask = &mainTask;
	hookIndex = arrivalPoint;
	hookAction = (countOnly) ? HookAction::none : HookAction::sbcRequestArrives;

	bool setKinematicsOk = false;
	mainTask.body = [&]
	{
		{
			MutexLocker gbLock(reprap.GetGCodes().mainGb.mutex);
			Hook("M669, before Move::SetKinematics");
			setKinematicsOk = move.SetKinematics("coreXY", 2);
			Hook("M669, after Move::SetKinematics");
		}
		if (sbc.state == TaskBase::State::blocked && !sbcExchange.answered)
		{
			Cpu::MakeReady(&sbc);				// the request arrives after M669, in the count run and after the last point
		}
	};
	SbcInterface sbcInterface;
	sbc.body = [&]
	{
		const PacketHeader packet = { (uint16_t)request, 0, 0, 0 };
		(void)sbcInterface.HandlePacket(&packet);
	};
	canReceiver.body = []
	{
		Free(Malloc(sizeof(float) * 12, "malloc in ExpansionManager::ProcessAnnouncement"), "free of the test's board data");
	};

	if (announcement)
	{
		RunTasks({ &mainTask, &sbc, &canReceiver });
	}
	else
	{
		RunTasks({ &mainTask, &sbc });
	}
	hookTask = sbcTask = canReceiverTask = nullptr;
	if (announcementPending)
	{
		announcementPending = false;
		Fail("the SBC task never read move.kinematics");
	}

	const unsigned int numPoints = mainTask.hooksPassed;
	if (!countOnly)
	{
		const std::string name = std::string((request == SbcRequest::SetVariable) ? "SetVariable" : "EvaluateExpression")
								+ " of move.kinematics.forwardMatrix arrives at Main's point '" + actionPoint + "' of M669"
								+ ((announcement) ? ", a board announces itself after the SBC task has read move.kinematics" : "");
		std::string why;
		bool ok = setKinematicsOk && move.kinematics == createdKinematics && kinematicsDeleted == 1 && move.kinematics != oldKinematics;
		if (!ok) { why = "M669 did not replace the kinematics exactly once"; }
		const bool oldValues = sbcExchange.result == Json(OldMatrix), newValues = sbcExchange.result == Json(NewMatrix);
		if (ok && !(sbcExchange.answered && (oldValues || newValues)))
		{
			ok = false;
			why = "DSF got '" + sbcExchange.result + "' instead of the matrix of the old or the new kinematics";
		}
		if (ok && request == SbcRequest::SetVariable)
		{
			// The tasks have finished, so read the variables without the lock: outside a task the lock code has no task to count its write locks on
			Variable *const v = reprap.globalVariables.GetVariables().Lookup("k", false);
			const std::vector<float> expected = { (oldValues) ? OldMatrix : NewMatrix, ((oldValues) ? OldMatrix : NewMatrix) + 1, ((oldValues) ? OldMatrix : NewMatrix) + 2 };
			if (v == nullptr || v->value != expected)
			{
				ok = false;
				why = "global.k does not hold the matrix that DSF was sent";
			}
		}
		Report(name, ok, why);
	}
	delete move.kinematics;
	move.kinematics = nullptr;
	FreeKeptMemory();
	return numPoints;
}

// Standalone. The network task (priority 1, as Main) reads ';CustomInfo t0 = tools[0].offsets' from a file for rr_fileinfo, and Main runs
// M563 P0 D-1 H-1 to delete tool 0. One of them starts, and at one of its points the time slice gives the processor to the other one,
// which runs until it has to wait or has finished. Returns the number of points that the starting task passes.
static unsigned int RunToolDeletion(bool networkFirst, unsigned int slicePoint, bool countOnly) noexcept
{
	ResetState();
	Tool::toolList = new Tool(0);
	GlobalVariables customInfo;
	FileInfoParser infoParser;
	infoParser.vars = &customInfo;

	TaskBase mainTask("Main", 1), network("NETWORK", 1);
	sbcTask = nullptr;
	hookTask = (networkFirst) ? &network : &mainTask;
	hookIndex = slicePoint;
	hookAction = (countOnly) ? HookAction::none : HookAction::timeSlice;

	mainTask.body = [&]
	{
		MutexLocker gbLock(reprap.GetGCodes().mainGb.mutex);
		Hook("M563, before Tool::DeleteTool");
		Tool::DeleteTool(0);
	};
	network.body = [&]
	{
		static const char line[] = "t0 = tools[0].offsets";
		Hook("rr_fileinfo, before FileInfoParser::ProcessCustomInfo");
		infoParser.ProcessCustomInfo("CustomInfo", line, line + sizeof(line) - 1, 0);
	};

	if (networkFirst)
	{
		RunTasks({ &network, &mainTask });
	}
	else
	{
		RunTasks({ &mainTask, &network });
	}
	hookTask = nullptr;

	const unsigned int numPoints = (networkFirst) ? network.hooksPassed : mainTask.hooksPassed;
	if (!countOnly)
	{
		const std::string name = std::string((networkFirst) ? "the network task reads custom info, time slice for M563 at its point '"
																: "M563 deletes tool 0, time slice for the network task's custom info at Main's point '")
								+ actionPoint + "'";
		std::string why;
		bool ok = Tool::toolList == nullptr && Tool::toolsDeleted == 1;
		if (!ok) { why = "M563 did not delete tool 0 exactly once"; }
		Variable *const v = customInfo.GetVariables().Lookup("t0", false);		// without the lock, as in RunKinematicsChange
		const std::vector<float> expected = { ToolOffsets, ToolOffsets + 1, ToolOffsets + 2 };
		// The parse finds tool 0 if it fetches tools[0] before M563 has taken the tool out of the list, and then the variable must hold its
		// offsets. Otherwise the parse fails, and ProcessCustomInfo stores nothing.
		if (ok && parseFoundTool && (v == nullptr || v->value != expected))
		{
			ok = false;
			why = "the custom info variable does not hold the offsets of tool 0, which the parse found";
		}
		if (ok && !parseFoundTool && v != nullptr)
		{
			ok = false;
			why = "a custom info variable was stored although the parse did not find tool 0";
		}
		Report(name, ok, why);
	}
	FreeKeptMemory();
	return numPoints;
}

int main()
{
	for (bool announcement : { false, true })
	{
		for (SbcRequest request : { SbcRequest::SetVariable, SbcRequest::EvaluateExpression })
		{
			const unsigned int numPoints = RunKinematicsChange(request, announcement, 0, true);
			for (unsigned int point = 0; point < numPoints; ++point)
			{
				(void)RunKinematicsChange(request, announcement, point, false);
			}
		}
	}

	for (bool networkFirst : { true, false })
	{
		const unsigned int numPoints = RunToolDeletion(networkFirst, 0, true);
		for (unsigned int point = 0; point < numPoints; ++point)
		{
			(void)RunToolDeletion(networkFirst, point, false);
		}
	}

	std::printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
