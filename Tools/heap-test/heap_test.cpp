// Host test of RRF's string heap: the real Platform/Heap.cpp and Platform/StringHandle.cpp, compiled unchanged.
//
// The main task deletes the string handles of the machine states, e.g. GCodeMachineState::CloseFile runs
// fname.Assign(nullptr) when a macro file ends. Another task can copy such a handle at the same time: the SBC task (and the
// network tasks without an SBC) serialises inputs[].currentFile, OBJECT_MODEL_FUNC(self->machineState->fname) in
// GCodeBuffer.cpp, into every live object model response, and ExpressionValue(AutoStringHandle) copies the handle with
// neither a lock nor a wait between reading the handle and increasing the reference count of its slot. All those tasks have a
// higher priority than the main task, so the main task never runs inside that copy, but the copy can run at any instruction
// of the main task.
//
// The test plays the other task at the moment the main task calls Heap::DeleteSlot for the handle and at the moment it
// returns from it (the link wraps Heap::DeleteSlot). If the handle still names its slot once the release has started, the copy
// takes a reference to a slot that has none left or is about to lose its last one, and the slot ends up with a reference
// count and no storage. On the board that is a MemoryProtectionFault in Heap::DeleteSlot (reading storage->length with
// storage == nullptr) or a failed RRF_ASSERT, whichever task releases the slot next.

#include <Platform/StringHandle.h>
#include <Platform/Heap.h>

#include <string>
#include <functional>

namespace Heap
{
	extern std::atomic<size_t> handlesUsed;					// defined in Heap.cpp, not declared in Heap.h
}

// RRF_ASSERT and the lock checks of the shim
static unsigned int assertFailures = 0;

void HeapTestAssertFailed(uint32_t line, const char *file) noexcept
{
	++assertFailures;
	fprintf(stderr, "    RRF_ASSERT failed in %s line %u\n", file, (unsigned int)line);
}

void ReadWriteLock::CheckHasWriteLock() noexcept
{
	if (writers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasWriteLock");
	}
}

void ReadWriteLock::CheckHasReadLock() noexcept
{
	if (readers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasReadLock");
	}
}

void ReadWriteLock::CheckHasReadOrWriteLock() noexcept
{
	if (readers == 0 && writers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasReadOrWriteLock");
	}
}

// Heap::DeleteSlot as the main task reaches it, linked with -Wl,--wrap=_ZN4Heap10DeleteSlotEPNS_9IndexSlotE
enum class Moment { callingDeleteSlot, returnedFromDeleteSlot };
static std::function<void(Moment)> otherTask;
static bool otherTaskRunning = false;

extern "C" void __real__ZN4Heap10DeleteSlotEPNS_9IndexSlotE(Heap::IndexSlot *slotPtr) noexcept;
extern "C" void __wrap__ZN4Heap10DeleteSlotEPNS_9IndexSlotE(Heap::IndexSlot *slotPtr) noexcept
{
	if (otherTaskRunning || !otherTask)
	{
		__real__ZN4Heap10DeleteSlotEPNS_9IndexSlotE(slotPtr);			// a release of the other task itself
		return;
	}
	otherTaskRunning = true;
	otherTask(Moment::callingDeleteSlot);
	otherTaskRunning = false;
	__real__ZN4Heap10DeleteSlotEPNS_9IndexSlotE(slotPtr);
	otherTaskRunning = true;
	otherTask(Moment::returnedFromDeleteSlot);
	otherTaskRunning = false;
}

// What the object model does with inputs[].currentFile: ExpressionValue(AutoStringHandle h) takes h by value and keeps
// h.IncreaseRefCount() in shVal, h goes out of scope, the JSON writer reads the string and ExpressionValue::Release deletes shVal
static std::string ReadAsObjectModel(const AutoStringHandle& published) noexcept
{
	StringHandle shVal;
	{
		const AutoStringHandle h(published);
		shVal = h.IncreaseRefCount();
	}
	std::string seen;
	if (assertFailures == 0)							// after a failed assertion the board would have reset, and Get() may fault
	{
		const ReadLockedPointer<const char> s = shVal.Get();
		seen = s.Ptr();
	}
	shVal.Delete();
	return seen;
}

static bool HeapIntact(std::string& why) noexcept
{
	String<StringLength256> msg;
	if (!Heap::CheckIntegrity(msg.GetRef()))
	{
		why = msg.c_str();
		return false;
	}
	return true;
}

// The ways in which the main task deletes a handle that another task may be reading
struct Deletion
{
	const char *name;
	std::function<void(AutoStringHandle*& handle, AutoStringHandle& other)> run;		// may delete *handle and set handle to nullptr
};

static const char *const FileName = "0:/sys/fan/check-idle.g";
static const char *const OtherName = "0:/sys/self-test/tick.g";

static const Deletion deletions[] =
{
	{ "Assign(nullptr), as CloseFile does",		[](AutoStringHandle*& h, AutoStringHandle&) { h->Assign(nullptr); } },
	{ "Assign(name), as StartNewFile does",		[](AutoStringHandle*& h, AutoStringHandle&) { h->Assign(OtherName); } },
	{ "Delete()",								[](AutoStringHandle*& h, AutoStringHandle&) { h->Delete(); } },
	{ "operator=",								[](AutoStringHandle*& h, AutoStringHandle& o) { *h = o; } },
	{ "destructor",								[](AutoStringHandle*& h, AutoStringHandle&) { delete h; h = nullptr; } },
};

static unsigned int failures = 0;

static void Report(bool ok, const std::string& test, const std::string& why) noexcept
{
	if (ok)
	{
		printf("PASS %s\n", test.c_str());
	}
	else
	{
		++failures;
		printf("FAIL %s: %s\n", test.c_str(), why.c_str());
	}
}

// Once the main task has started to release the slot, the handle must no longer name it. That covers every instruction of
// the release, including the ones between the decrement of the reference count and the reads of storage in DeleteSlot
static void HandleIsClearedBeforeItsSlotIsReleased(const Deletion& d) noexcept
{
	assertFailures = 0;
	AutoStringHandle other(OtherName);
	AutoStringHandle *handle = new AutoStringHandle(FileName);
	AutoStringHandle *const watched = handle;
	bool namedAtRelease = false;
	otherTask = [&](Moment m)
	{
		if (m == Moment::callingDeleteSlot && !watched->IsNull())
		{
			namedAtRelease = true;
		}
	};
	d.run(handle, other);
	otherTask = nullptr;
	delete handle;

	Report(!namedAtRelease && assertFailures == 0, std::string("handle is cleared before its slot is released: ") + d.name,
			namedAtRelease ? "the handle still names the slot when Heap::DeleteSlot is called" : "RRF_ASSERT failed");
}

// The other task copies the handle while the main task releases it, as the object model does with inputs[].currentFile
static void ObjectModelReadDuringTheRelease(const Deletion& d, Moment when) noexcept
{
	assertFailures = 0;
	const size_t handlesBefore = Heap::handlesUsed;
	std::string seen = "(not read)";
	{
		AutoStringHandle other(OtherName);
		AutoStringHandle *handle = new AutoStringHandle(FileName);
		AutoStringHandle *const watched = handle;
		otherTask = [&](Moment m)
		{
			if (m == when)
			{
				seen = ReadAsObjectModel(*watched);
			}
		};
		d.run(handle, other);
		otherTask = nullptr;
		delete handle;
	}

	std::string why;
	bool ok = true;
	if (assertFailures != 0)
	{
		ok = false;
		why = "RRF_ASSERT failed, the board resets here";
	}
	else if (seen != FileName && !seen.empty())
	{
		ok = false;
		why = "the object model read \"" + seen + "\"";
	}
	else if (!HeapIntact(why))
	{
		ok = false;
	}
	else if (Heap::handlesUsed != handlesBefore)
	{
		ok = false;
		why = std::to_string(Heap::handlesUsed - handlesBefore) + " handles still in use after every handle was deleted";
	}
	Report(ok, std::string("object model read ") + ((when == Moment::callingDeleteSlot) ? "as the release starts: " : "as the release returns: ") + d.name, why);
}

int main() noexcept
{
	for (const Deletion& d : deletions)
	{
		HandleIsClearedBeforeItsSlotIsReleased(d);
	}
	for (const Deletion& d : deletions)
	{
		ObjectModelReadDuringTheRelease(d, Moment::callingDeleteSlot);
		ObjectModelReadDuringTheRelease(d, Moment::returnedFromDeleteSlot);
	}
	printf("%u failed\n", failures);
	return (failures == 0) ? 0 : 1;
}
