// Host test of RRF's string heap: the real Platform/Heap.cpp, Platform/StringHandle.cpp and Platform/ArrayHandle.cpp, compiled
// unchanged (ExpressionValue is the stand-in in shim/ObjectModel/ObjectModel.h).
//
// The main task deletes the string handles of the machine states, e.g. GCodeMachineState::CloseFile runs
// fname.Assign(nullptr) when a macro file ends. Another task can copy such a handle at the same time: the SBC task serialises
// inputs[].currentFile, OBJECT_MODEL_FUNC(self->machineState->fname) in GCodeBuffer.cpp, into every live object model response,
// and ExpressionValue(AutoStringHandle) copies the handle with neither a lock nor a wait between reading the handle and
// increasing the reference count of its slot. The SBC task has a higher priority than the main task, so the main task never
// runs inside that copy, but the copy can run at any instruction of the main task. Without an SBC the network task makes the
// same copy at the main task's priority with time slicing, so the main task can run inside it; the order checked here does not
// protect that reader (Tools/pointer-check/census.md). Array handles are copied the same way, by
// ExpressionValue(const ExpressionValue&).
//
// The test plays the other task at the moment the main task calls Heap::DeleteSlot and at the moment it returns from it (the
// link wraps Heap::DeleteSlot). If the handle still names its slot once the release has started, the copy takes a reference to
// a slot that has none left or is about to lose its last one, and the slot ends up with a reference count and no storage. On
// the board that is a MemoryProtectionFault in Heap::DeleteSlot (reading storage->length with storage == nullptr), in
// ArrayHandle::Delete (reading storage->count) or a failed RRF_ASSERT, whichever task releases the slot next. An array copy taken
// while the main task releases the contents of the array also sees its elements vanish.

#include <Platform/StringHandle.h>
#include <Platform/ArrayHandle.h>
#include <Platform/Heap.h>
#include <ObjectModel/ObjectModel.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <functional>
#include <vector>

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

// Arrays: an array of two heap strings, built as ExpressionParser does (Allocate and AssignElement under the write lock)
static const std::string ArrayContents = std::string(FileName) + "|" + OtherName;

static AutoArrayHandle *NewArray() noexcept
{
	const ExpressionValue first{StringHandle(FileName)}, second{StringHandle(OtherName)};
	AutoArrayHandle *const array = new AutoArrayHandle;
	WriteLocker locker(Heap::heapLock);
	array->Allocate(2);
	array->AssignElement(0, const_cast<ExpressionValue&>(first));
	array->AssignElement(1, const_cast<ExpressionValue&>(second));
	return array;
}

// What the object model does with an array, e.g. a global variable: ExpressionValue(const ExpressionValue&) copies the handle and
// increases the reference count of its slot, the JSON writer reads the elements under a read lock and the copy is released.
// The arrays of this test always have two elements, so none means that the copy names a slot without storage. Its release would
// read storage->count, which is the MemoryProtectionFault on the board, so the copy keeps its reference instead.
static std::string ReadArrayAsObjectModel(const ArrayHandle& published, bool& revived) noexcept
{
	ArrayHandle copy = published;
	copy.IncreaseRefCount();
	if (copy.IsNull())
	{
		return "";
	}

	std::string seen;
	size_t numElements;
	{
		ReadLocker locker(Heap::heapLock);
		numElements = copy.GetNumElements();
		for (size_t i = 0; i < numElements; ++i)
		{
			ExpressionValue element;
			if (copy.GetElement(i, element) && element.GetType() == TypeCode::HeapString)
			{
				seen += element.shVal.Get().Ptr();
			}
			if (i + 1 < numElements)
			{
				seen += '|';
			}
		}
	}
	if (numElements == 0)
	{
		revived = true;
		return "";
	}
	copy.Delete();
	return seen;
}

struct ArrayDeletion
{
	const char *name;
	std::function<void(AutoArrayHandle*& handle, AutoArrayHandle& other)> run;		// may delete *handle and set handle to nullptr
};

static const ArrayDeletion arrayDeletions[] =
{
	{ "Delete()",								[](AutoArrayHandle*& h, AutoArrayHandle&) { h->Delete(); } },
	{ "operator=",								[](AutoArrayHandle*& h, AutoArrayHandle& o) { *h = o; } },
	{ "destructor",								[](AutoArrayHandle*& h, AutoArrayHandle&) { delete h; h = nullptr; } },
};

// The release of an array starts with the release of its contents, which calls Heap::DeleteSlot for every heap string in it
static void ArrayHandleIsClearedBeforeItsSlotIsReleased(const ArrayDeletion& d) noexcept
{
	assertFailures = 0;
	AutoArrayHandle *const other = NewArray();
	AutoArrayHandle *handle = NewArray();
	AutoArrayHandle *const watched = handle;
	bool namedAtRelease = false;
	otherTask = [&](Moment m)
	{
		if (m == Moment::callingDeleteSlot && !watched->IsNull())
		{
			namedAtRelease = true;
		}
	};
	d.run(handle, *other);
	otherTask = nullptr;
	delete handle;
	delete other;

	Report(!namedAtRelease && assertFailures == 0, std::string("array handle is cleared before its slot is released: ") + d.name,
			namedAtRelease ? "the handle still names the array when Heap::DeleteSlot is called" : "RRF_ASSERT failed");
}

// The other task copies the array handle while the main task releases it. The copy must see the whole array or none of it
static void ArrayReadDuringTheRelease(const ArrayDeletion& d, Moment when) noexcept
{
	assertFailures = 0;
	const size_t handlesBefore = Heap::handlesUsed;
	bool revived = false;
	std::vector<std::string> seen;
	{
		AutoArrayHandle *const other = NewArray();
		AutoArrayHandle *handle = NewArray();
		AutoArrayHandle *const watched = handle;
		otherTask = [&](Moment m)
		{
			if (m == when && !revived)
			{
				seen.push_back(ReadArrayAsObjectModel(*watched, revived));
			}
		};
		d.run(handle, *other);
		otherTask = nullptr;
		delete handle;
		delete other;
	}

	std::string why;
	bool ok = true;
	if (assertFailures != 0)
	{
		ok = false;
		why = "RRF_ASSERT failed, the board resets here";
	}
	else if (revived)
	{
		ok = false;
		why = "the copy revived a released array, its release faults on the board";
	}
	else
	{
		for (const std::string& s : seen)
		{
			if (s != ArrayContents && !s.empty())
			{
				ok = false;
				why = "the object model read the array as \"" + s + "\" while its contents were released";
				break;
			}
		}
		if (ok && !HeapIntact(why))
		{
			ok = false;
		}
		else if (ok && Heap::handlesUsed != handlesBefore)
		{
			ok = false;
			why = std::to_string(Heap::handlesUsed - handlesBefore) + " handles still in use after every handle was deleted";
		}
	}
	Report(ok, std::string("array read ") + ((when == Moment::callingDeleteSlot) ? "as a release starts: " : "as a release returns: ") + d.name, why);
}

// ArrayHandle::Allocate computes the size of the array as sizeof(ArrayStorageSpace) + numElements * sizeof(ExpressionValue). On the board
// (32-bit size_t, 8-byte ExpressionValue) vector(536870912, 0) makes that 4, the check of the space passes, and the loop writes 2^29
// elements beyond a space of 4 bytes. With the 64-bit size_t of the host the same wrap needs 2^64 / sizeof(ExpressionValue) elements.
// Allocate must refuse both counts with "Array too large" before it touches the heap
static void ArrayWhoseSizeWrapsAroundIsRefused(size_t numElements, const char *what) noexcept
{
	std::string why;
	{
		AutoArrayHandle array;
		WriteLocker locker(Heap::heapLock);
		try
		{
			array.Allocate(numElements);
			why = "the array was allocated";
		}
		catch (const GCodeException& e)
		{
			if (strcmp(e.GetMessage(), "Array too large") != 0) { why = std::string("exception \"") + e.GetMessage() + "\""; }
		}
	}
	Report(why.empty(), std::string("an array of ") + std::to_string(numElements) + " elements (" + what + ") is refused", why);
}

int main() noexcept
{
	ArrayWhoseSizeWrapsAroundIsRefused(SIZE_MAX / sizeof(ExpressionValue) + 1, "its size wraps around to 4 on the host");
	ArrayWhoseSizeWrapsAroundIsRefused(536870912, "the count that wraps around on the board");
	ArrayWhoseSizeWrapsAroundIsRefused(SIZE_MAX, "the largest count");

	for (const Deletion& d : deletions)
	{
		HandleIsClearedBeforeItsSlotIsReleased(d);
	}
	for (const Deletion& d : deletions)
	{
		ObjectModelReadDuringTheRelease(d, Moment::callingDeleteSlot);
		ObjectModelReadDuringTheRelease(d, Moment::returnedFromDeleteSlot);
	}
	for (const ArrayDeletion& d : arrayDeletions)
	{
		ArrayHandleIsClearedBeforeItsSlotIsReleased(d);
	}
	for (const ArrayDeletion& d : arrayDeletions)
	{
		ArrayReadDuringTheRelease(d, Moment::callingDeleteSlot);
		ArrayReadDuringTheRelease(d, Moment::returnedFromDeleteSlot);
	}
	printf("%u failed\n", failures);
	return (failures == 0) ? 0 : 1;
}
