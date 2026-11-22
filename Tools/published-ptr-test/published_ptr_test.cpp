// Host test of the pointer class PublishedPtr: src/Platform/PublishedPtr.h and src/Platform/PublishedPtr.cpp, compiled unchanged, with the
// real ReadWriteLock of RRFLibraries (rwlock.cpp) on the host RTOS of shim/HostRtos.h, and the task priorities of src/Platform/TaskPriorities.h.
//
// The tasks of the test are the ones that use Move::kinematics and GCodeBuffer::machineState on the board: the main task is the writer
// (SpinPriority), the network task has the same priority (Network.cpp:519) and time slicing, the SBC and Move tasks have higher ones.
// A thread plays a task after HostRtos::SetCurrentTask. A deleted TestObject keeps its memory until the end of the test, so that a reader
// that still holds it reads defined memory and the test can report it, and every deletion of an object is counted.
//
// Built with PUBLISHED_PTR_TEST_NO_CHECKS, RRF_ASSERT and RTOS_ASSERT do nothing, which takes out the checks of R1 (~Publishable), of the
// writer (CheckWriter) and of R3 (CheckHasReadOrWriteLock). The cases that depend on them must then fail and all others must pass.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define HAS_SBC_INTERFACE	1										// for TaskPriority::SbcPriority
#include <RepRapFirmware.h>											// the stand-in in shim/
#include <Platform/PublishedPtr.h>
#include <Platform/TaskPriorities.h>

// Assertions that the code under test makes
struct AssertRecord
{
	std::string file;
	uint32_t line;
};

static std::mutex assertsMutex;
static std::vector<AssertRecord> asserts;

void TestAssertFailed(uint32_t line, const char *file) noexcept
{
	std::lock_guard<std::mutex> lock(assertsMutex);
	asserts.push_back(AssertRecord{file, line});
}

static unsigned int AssertsIn(const char *fileEnd) noexcept
{
	std::lock_guard<std::mutex> lock(assertsMutex);
	unsigned int n = 0;
	for (const AssertRecord& a : asserts)
	{
		if (a.file.size() >= strlen(fileEnd) && a.file.compare(a.file.size() - strlen(fileEnd), std::string::npos, fileEnd) == 0)
		{
			++n;
		}
	}
	return n;
}

static std::string AssertsText() noexcept
{
	std::lock_guard<std::mutex> lock(assertsMutex);
	std::string s;
	for (const AssertRecord& a : asserts)
	{
		s += std::string((s.empty()) ? "" : ", ") + a.file + ":" + std::to_string(a.line);
	}
	return (s.empty()) ? "none" : s;
}

// The tasks
static TaskBase mainTask, networkTask, secondSpinTask, sbcTask, moveTask, idleTask;

class AsTask
{
public:
	explicit AsTask(TaskBase& t) noexcept : before(RTOSIface::GetCurrentTask()) { HostRtos::SetCurrentTask(&t); }
	~AsTask() noexcept { HostRtos::SetCurrentTask(before); }

private:
	TaskBase *before;
};

// The objects that the cases publish
static std::mutex objectsMutex;
static std::set<void *> deletedObjects;
static std::map<const void *, unsigned int> deletions;

class TestObject : public Publishable
{
public:
	TestObject(const char *lbl, TestObject *prev) noexcept : previous(prev), label(lbl), alive(true) { }
	~TestObject() noexcept
	{
		alive = false;
		if (whileDestroying != nullptr)
		{
			whileDestroying(this);
		}
	}

	static void *operator new(size_t sz) { return ::operator new(sz); }
	static void operator delete(void *p) noexcept
	{
		std::lock_guard<std::mutex> lock(objectsMutex);
		++deletions[p];
		deletedObjects.insert(p);
	}

	TestObject *previous;
	const char *label;
	bool alive;

	static void (*whileDestroying)(TestObject *t) noexcept;
};

void (*TestObject::whileDestroying)(TestObject *t) noexcept = nullptr;

static bool IsDeleted(const void *p) noexcept
{
	std::lock_guard<std::mutex> lock(objectsMutex);
	return deletedObjects.count(const_cast<void *>(p)) != 0;
}

static unsigned int Deletions(const void *p) noexcept
{
	std::lock_guard<std::mutex> lock(objectsMutex);
	const auto it = deletions.find(p);
	return (it == deletions.end()) ? 0 : it->second;
}

// Cases
static std::vector<std::string> failures;
static unsigned int numCases = 0, numFailed = 0, numNeedingChecks = 0, numFailedNeedingChecks = 0;

static void Check(bool ok, const std::string& what) noexcept
{
	if (!ok)
	{
		failures.push_back(what);
	}
}

static void CheckNoAssertion() noexcept
{
	Check(asserts.empty(), "unexpected assertion at " + AssertsText());
}

static void Run(const char *name, bool needsChecks, void (*body)() noexcept) noexcept
{
	failures.clear();
	asserts.clear();
	HostRtos::SetCurrentTask(&mainTask);
	HostRtos::SetInInterrupt(false);
	HostRtos::SetBasePri(0);
	HostRtos::SetPriMask(0);
	TestObject::whileDestroying = nullptr;

	body();

	++numCases;
	if (needsChecks)
	{
		++numNeedingChecks;
	}
	if (failures.empty())
	{
		std::printf("PASS  %s\n", name);
		return;
	}
	++numFailed;
	if (needsChecks)
	{
		++numFailedNeedingChecks;
	}
	std::printf("FAIL  %s\n", name);
	for (const std::string& f : failures)
	{
		std::printf("        %s\n", f.c_str());
	}
}

// R1: deleting an object while a PublishedPtr points to it is caught by ~Publishable
static void DeletePublished() noexcept
{
	// The PublishedPtr is left as it is afterwards, because it points to the deleted object
	alignas(PublishedPtr<TestObject>) static unsigned char storage[sizeof(PublishedPtr<TestObject>)];
	PublishedPtr<TestObject>& p = *new (storage) PublishedPtr<TestObject>(new TestObject("a", nullptr), &mainTask);

	delete p.Get();
	const bool caught = AssertsIn("Platform/PublishedPtr.h") == 1 && asserts.size() == 1;

	// What a reader that outranks the main task finds next, e.g. the SBC task serialising move.kinematics
	bool readerGetsDeleted;
	{
		AsTask as(sbcTask);
		readerGetsDeleted = IsDeleted(p.Get());
	}
	Check(caught, std::string("the deletion is not caught (assertions: ") + AssertsText() + ")"
					+ ((readerGetsDeleted) ? "; the SBC task then gets the deleted object from the PublishedPtr" : ""));
}

// R1: Replace points to the new object first, then deletes the old one once, after it has released the write lock (as ReplaceObject and
// as Move::SetKinematics did before, #51)
static PublishedPtr<TestObject> *observed = nullptr;
static const TestObject *seenWhileDeleting = nullptr;
static bool writeLockedWhileDeleting = false;

static void Observe(TestObject *) noexcept
{
	seenWhileDeleting = observed->Get();
	TaskCriticalSectionLocker lock;									// IsWriteLocked must be called with scheduling disabled
	writeLockedWhileDeleting = observed->GetLock().IsWriteLocked();
}

static void ReplaceDeletesOnce() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	TestObject *const a = p.Get();
	TestObject *const b = new TestObject("b", nullptr);

	observed = &p;
	seenWhileDeleting = nullptr;
	writeLockedWhileDeleting = true;
	TestObject::whileDestroying = Observe;
	p.Replace(b);
	TestObject::whileDestroying = nullptr;

	Check(Deletions(a) == 1, "the old object is deleted " + std::to_string(Deletions(a)) + " times");
	Check(seenWhileDeleting == b, "while the old object is destroyed the PublishedPtr does not point to the new one");
	Check(!writeLockedWhileDeleting, "the old object is destroyed while the write lock is still held");
	Check(p.Get() == b && !IsDeleted(b), "afterwards the PublishedPtr does not point to the intact new object");
	CheckNoAssertion();
	p.Delete();
}

// R1: Delete deletes the object once and leaves the PublishedPtr pointing to nothing
static void DeleteDeletesOnce() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	TestObject *const a = p.Get();
	p.Delete();
	Check(Deletions(a) == 1, "the object is deleted " + std::to_string(Deletions(a)) + " times");
	Check(p.Get() == nullptr, "the PublishedPtr still points to an object");
	p.Delete();														// nothing to delete
	Check(Deletions(a) == 1, "a second Delete deletes the object again");
	CheckNoAssertion();
}

// Publish keeps the old object and hands it back. It is no longer published, so its owner may delete it.
static void PublishKeepsOld() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	TestObject *const a = p.Get();
	TestObject *const b = new TestObject("b", nullptr);
	TestObject *const ret = p.Publish(b);
	Check(ret == a && !IsDeleted(a) && a->alive, "Publish does not hand back the intact old object");
	Check(p.Get() == b, "the PublishedPtr does not point to the new object");
	delete a;
	CheckNoAssertion();
	p.Delete();
}

// Only the writer may change what a PublishedPtr points to
static void OnlyTheWriterReplaces() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	{
		AsTask as(networkTask);
		p.Replace(new TestObject("b", nullptr));
	}
	Check(AssertsIn("Platform/PublishedPtr.h") == 1 && asserts.size() == 1,
			"a Replace by the network task is not caught (assertions: " + AssertsText() + ")");
	p.Delete();
}

// R3: readers that the writer cannot interrupt read without the lock. The cheap cases do not query the priorities.
static void WriterReads() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	const unsigned int queries = HostRtos::GetPriorityQueries();
	(void)p.Get();
	(void)p->label;
	(void)(*p).label;
	CheckNoAssertion();
	Check(HostRtos::GetPriorityQueries() == queries, "the writer's read queries the priorities");
	p.Delete();
}

static void IsrReads() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	const unsigned int queries = HostRtos::GetPriorityQueries();
	{
		AsTask as(networkTask);										// the task that the ISR interrupted
		HostRtos::SetInInterrupt(true);
		(void)p.Get();
		HostRtos::SetInInterrupt(false);
	}
	CheckNoAssertion();
	Check(HostRtos::GetPriorityQueries() == queries, "the read in an ISR queries the priorities");
	p.Delete();
}

static void ReadsWithSchedulerSuspended() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	const unsigned int queries = HostRtos::GetPriorityQueries();
	{
		AsTask as(networkTask);										// e.g. urgent M25.1 or M24 in DDARing::Hold or ReleaseHold (DDARing.cpp:845, :940)
		TaskCriticalSectionLocker lock;
		(void)p.Get();
	}
	CheckNoAssertion();
	Check(HostRtos::GetPriorityQueries() == queries, "the read with the scheduler suspended queries the priorities");
	p.Delete();
}

static void ReadsWithInterruptsMasked() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	const unsigned int queries = HostRtos::GetPriorityQueries();
	{
		AsTask as(networkTask);										// e.g. Move::OnEndstopOrZProbeStatesChanged under a BasePriorityBooster (Move.cpp:4121)
		HostRtos::SetBasePri(0x20);
		(void)p.Get();
		Check(HostRtos::GetBasePri() == 0x20, "the read sets BASEPRI to " + std::to_string(HostRtos::GetBasePri()));

		// The host RTOS does what the board does when the priority is queried here, which is why the check must not do it
		(void)TaskBase::GetCurrentTaskPriority();
		Check(HostRtos::GetBasePri() == 0, "host RTOS: a priority query leaves BASEPRI set, unlike vPortExitCritical");
		HostRtos::SetBasePri(0);

		HostRtos::SetPriMask(1);
		(void)p.Get();
		HostRtos::SetPriMask(0);
	}
	CheckNoAssertion();
	Check(HostRtos::GetPriorityQueries() == queries + 1, "the reads with interrupts masked query the priorities");
	p.Delete();
}

static void HigherPriorityReads() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	{
		AsTask as(sbcTask);
		(void)p.Get();
	}
	{
		AsTask as(moveTask);
		(void)p.Get();
	}
	CheckNoAssertion();
	p.Delete();
}

// The writer runs at its base priority unless it holds a mutex that a task with a higher priority waits for. The check compares with
// the base priority, so that the SBC task does not fail meanwhile.
static void WriterWithInheritedPriority() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	mainTask.uxPriority = TaskPriority::MovePriority;
	{
		AsTask as(sbcTask);
		(void)p.Get();
	}
	mainTask.uxPriority = mainTask.uxBasePriority;
	CheckNoAssertion();
	p.Delete();
}

// R3: a reader at the writer's priority must hold the lock. Without it the main task can run in a time slice and delete the object.
static void SamePriorityWithoutLock() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	const TestObject *seen;
	{
		AsTask as(networkTask);
		seen = p.Get();
	}
	const bool caught = AssertsIn("RTOSIface/RTOSIface.cpp") == 1 && asserts.size() == 1;

	// The main task gets the next time slice and replaces the object
	p.Replace(new TestObject("b", nullptr));
	Check(caught, std::string("the read is not caught (assertions: ") + AssertsText() + ")"
					+ ((IsDeleted(seen)) ? "; the main task then deletes the object that the network task holds" : ""));
	p.Delete();
}

static void BelowWriterPriorityWithoutLock() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	{
		AsTask as(idleTask);
		(void)p.Get();
	}
	Check(AssertsIn("RTOSIface/RTOSIface.cpp") == 1 && asserts.size() == 1, "the read is not caught (assertions: " + AssertsText() + ")");
	p.Delete();
}

static void SamePriorityWithLock() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	{
		AsTask as(networkTask);
		ReadLocker lock(p.GetLock());
		(void)p.Get();
		(void)p->label;
	}
	CheckNoAssertion();
	p.Delete();
}

// The read lock counts only for the task that holds it
static void LockOfAnotherTask() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	{
		AsTask as(networkTask);
		ReadLocker lock(p.GetLock());
		AsTask as2(secondSpinTask);
		(void)p.Get();
	}
	Check(AssertsIn("RTOSIface/RTOSIface.cpp") == 1 && asserts.size() == 1, "the read is not caught (assertions: " + AssertsText() + ")");
	p.Delete();
}

// The machine state stack of a GCodeBuffer: PushState publishes a new top that links to the old one, PopState replaces the top with the
// previous level, ForkFrom publishes a forked chain and deletes the old chain from the top down
static unsigned int Depth(const PublishedPtr<TestObject>& stack, bool& allAlive) noexcept
{
	unsigned int depth = 0;
	allAlive = true;
	for (const TestObject *t = stack.Get(); t != nullptr; t = t->previous)
	{
		++depth;
		allAlive = allAlive && !IsDeleted(t) && t->alive;
	}
	return depth;
}

static void PushAndPop() noexcept
{
	PublishedPtr<TestObject> stack(new TestObject("base", nullptr), &mainTask);
	TestObject *const base = stack.Get();
	TestObject *const ret1 = stack.Publish(new TestObject("macro 1", stack.Get()));
	TestObject *const m1 = stack.Get();
	TestObject *const ret2 = stack.Publish(new TestObject("macro 2", m1));
	TestObject *const m2 = stack.Get();
	Check(ret1 == base && ret2 == m1, "a push does not hand back the previous top");

	bool allAlive;
	unsigned int depth;
	{
		AsTask as(sbcTask);
		depth = Depth(stack, allAlive);
	}
	Check(depth == 3 && allAlive, "after two pushes the SBC task does not find three intact levels");

	stack.Replace(stack.Get()->previous);
	Check(Deletions(m2) == 1 && stack.Get() == m1 && !IsDeleted(m1), "the first pop does not delete the top once and publish the level below");
	stack.Replace(stack.Get()->previous);
	Check(Deletions(m1) == 1 && stack.Get() == base && !IsDeleted(base), "the second pop does not delete the top once and publish the base");
	{
		AsTask as(networkTask);
		ReadLocker lock(stack.GetLock());
		depth = Depth(stack, allAlive);
	}
	Check(depth == 1 && allAlive, "after the pops the network task does not find the intact base alone");

	TestObject *const f1 = new TestObject("fork base", nullptr);
	TestObject *const f2 = new TestObject("fork macro", f1);
	TestObject *chain = stack.Publish(f2);
	while (chain != nullptr)
	{
		TestObject *const toDelete = chain;
		chain = chain->previous;
		delete toDelete;
	}
	Check(Deletions(base) == 1 && stack.Get() == f2, "the fork does not delete the old chain once and publish the new one");
	CheckNoAssertion();

	stack.Replace(stack.Get()->previous);
	stack.Delete();
	Check(Deletions(f1) == 1 && Deletions(f2) == 1, "the forked chain is not deleted once per level");
	CheckNoAssertion();
}

// Replace waits until a reader that holds the read lock has finished with the object
static bool WaitFor(bool (*condition)() noexcept) noexcept
{
	for (unsigned int i = 0; i < 5000; ++i)
	{
		if (condition())
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

static std::atomic<int> phase(0);

static void ReplaceWaitsForReader() noexcept
{
	PublishedPtr<TestObject> p(new TestObject("a", nullptr), &mainTask);
	TestObject *const a = p.Get();
	TestObject *const b = new TestObject("b", nullptr);
	bool readerFoundIntact = false;

	phase = 0;
	std::thread reader([&p, &readerFoundIntact]() noexcept
		{
			HostRtos::SetCurrentTask(&networkTask);
			ReadLocker lock(p.GetLock());
			const TestObject *const seen = p.Get();
			phase = 1;
			while (phase.load() != 2)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			readerFoundIntact = !IsDeleted(seen) && seen->alive;
		});
	const bool readerHolds = WaitFor([]() noexcept { return phase.load() == 1; });

	std::thread writer([&p, b]() noexcept
		{
			HostRtos::SetCurrentTask(&mainTask);
			p.Replace(b);
		});
	const bool writerWaits = WaitFor([]() noexcept { return HostRtos::IsWaitingForNotification(&mainTask); });

	// Meanwhile an ISR finds the old object, intact
	HostRtos::SetInInterrupt(true);
	const TestObject *const seenMeanwhile = p.Get();
	HostRtos::SetInInterrupt(false);
	const bool intactMeanwhile = !IsDeleted(a) && a->alive;

	phase = 2;
	reader.join();
	writer.join();

	Check(readerHolds, "the reader does not get the read lock");
	Check(writerWaits, "Replace does not wait for the reader");
	Check(seenMeanwhile == a && intactMeanwhile, "while Replace waits, the PublishedPtr does not point to the intact old object");
	Check(readerFoundIntact, "the reader finds its object deleted before it releases the lock");
	Check(Deletions(a) == 1 && p.Get() == b, "afterwards the old object is not deleted once or the new one is not published");
	CheckNoAssertion();
	p.Delete();
}

int main()
{
	vTaskPrioritySet(mainTask.GetFreeRTOSHandle(), TaskPriority::SpinPriority);
	vTaskPrioritySet(networkTask.GetFreeRTOSHandle(), TaskPriority::SpinPriority);
	vTaskPrioritySet(secondSpinTask.GetFreeRTOSHandle(), TaskPriority::SpinPriority);
	vTaskPrioritySet(sbcTask.GetFreeRTOSHandle(), TaskPriority::SbcPriority);
	vTaskPrioritySet(moveTask.GetFreeRTOSHandle(), TaskPriority::MovePriority);
	vTaskPrioritySet(idleTask.GetFreeRTOSHandle(), TaskPriority::IdlePriority);

	Run("R1: deleting an object that a PublishedPtr points to is caught", true, DeletePublished);
	Run("R1: Replace publishes the new object, then deletes the old one once without the write lock", false, ReplaceDeletesOnce);
	Run("R1: Delete deletes the object once and leaves the PublishedPtr empty", false, DeleteDeletesOnce);
	Run("Publish hands back the old object intact and unpublished", false, PublishKeepsOld);
	Run("a Replace by a task other than the writer is caught", true, OnlyTheWriterReplaces);
	Run("R3: the writer reads without the lock and without a priority query", false, WriterReads);
	Run("R3: an ISR reads without the lock and without a priority query", false, IsrReads);
	Run("R3: a task reads without the lock while the scheduler is suspended", false, ReadsWithSchedulerSuspended);
	Run("R3: a task reads without the lock while interrupts are masked, and BASEPRI stays set", false, ReadsWithInterruptsMasked);
	Run("R3: the SBC and Move tasks read without the lock", false, HigherPriorityReads);
	Run("R3: the SBC task reads without the lock while the main task has inherited a higher priority", false, WriterWithInheritedPriority);
	Run("R3: the network task reading without the lock is caught", true, SamePriorityWithoutLock);
	Run("R3: a task below the writer's priority reading without the lock is caught", true, BelowWriterPriorityWithoutLock);
	Run("R3: the network task reads with the read lock", false, SamePriorityWithLock);
	Run("R3: a read lock held by another task does not count", true, LockOfAnotherTask);
	Run("machine state stack: push, pop and fork publish and delete each level once", false, PushAndPop);
	Run("Replace waits until a reader that holds the read lock has finished", false, ReplaceWaitsForReader);

	for (void *p : deletedObjects)
	{
		::operator delete(p);
	}

#ifdef PUBLISHED_PTR_TEST_NO_CHECKS
	std::printf("%u of %u failed without the checks; %u cases depend on them\n", numFailed, numCases, numNeedingChecks);
	return (numFailed == numNeedingChecks && numFailedNeedingChecks == numNeedingChecks) ? 0 : 1;
#else
	std::printf("%u of %u failed\n", numFailed, numCases);
	return (numFailed == 0) ? 0 : 1;
#endif
}
