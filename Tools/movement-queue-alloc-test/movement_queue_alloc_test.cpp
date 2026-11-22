// Host test of the allocation of new DDAs by M595 P (DDARing::ConfigureMovementQueue in src/Movement/DDARing.cpp), compiled from the
// real text of the function that the Makefile extracts.
//
// DDA::operator new is Tasks::AllocPermanent (src/Movement/DDA.h), which takes the malloc mutex through GetMallocMutex and then
// lowers the heap limit (CoreAllocPermanent in CoreN2G). GetMallocMutex takes the mutex only while the scheduler is running
// (src/Platform/Tasks.cpp), so an AllocPermanent with the scheduler suspended runs without it, while a task of the same or a lower
// priority may have been preempted inside malloc, which reads the heap limit in _sbrk. M595 runs in the task of the G-code channel,
// the main task. Up to 3.7-dev de1bfe598, ConfigureMovementQueue allocated each new DDA inside its TaskCriticalSectionLocker, which
// suspends the scheduler.
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h) with a counter and AllocPermanent with the host heap. Every
// allocation must happen with the scheduler running. It also checks what M595 does: the ring grows to the number of DDAs asked for,
// stays a consistent ring in both directions, and addPointer and the DDA before it stay the same; a smaller number leaves the ring
// alone; too little RAM gives an error without allocating; M595 without P or R reports the settings.

#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#define _ecv_null
#define THROWS(...)

enum class GCodeResult { notFinished, ok, error };

static unsigned int suspended = 0;							// uxSchedulerSuspended
static std::vector<std::string> failures;
static std::vector<void *> permanent;							// what AllocPermanent handed out
static ptrdiff_t neverUsedRam = 100000;
static bool standstill = true;
static unsigned int moveUpdates = 0;

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

namespace Tasks
{
	ptrdiff_t GetNeverUsedRam() noexcept { return neverUsedRam; }

	void *AllocPermanent(size_t sz) noexcept
	{
		if (suspended != 0)
		{
			failures.push_back("Tasks::AllocPermanent was called with the scheduler suspended");
		}
		void *const p = malloc(sz);
		permanent.push_back(p);
		return p;
	}
}

// Just what ConfigureMovementQueue uses of DDA (src/Movement/DDA.h), with its operator new and delete
class DDA
{
public:
	explicit DDA(DDA *_ecv_null n) noexcept : next(n), prev(nullptr) { }

	void *operator new(size_t count) { return Tasks::AllocPermanent(count); }
	void operator delete(void *) noexcept { }

	void SetNext(DDA *n) noexcept { next = n; }
	void SetPrevious(DDA *p) noexcept { prev = p; }
	DDA *GetNext() const noexcept { return next; }
	DDA *GetPrevious() const noexcept { return prev; }

private:
	DDA *_ecv_null next;
	DDA *_ecv_null prev;
	uint32_t payload[64];									// a DDA is large, so sizeof(DDA) in the RAM estimate is not zero
};

// Just what ConfigureMovementQueue uses of GCodeBuffer, StringRef, GCodes and RepRap
class GCodeBuffer
{
public:
	bool hasP = false, hasR = false;
	uint32_t p = 0, r = 0;

	bool TryGetUIValue(char c, uint32_t& val, bool& seen) noexcept
	{
		if ((c == 'P' && hasP) || (c == 'R' && hasR))
		{
			val = (c == 'P') ? p : r;
			seen = true;
			return true;
		}
		return false;
	}
};

// StringRef::printf has no format attribute here: the "insufficient RAM" reply prints a ptrdiff_t with %d, which is right on the
// board (32 bits) and would only warn on the host
class StringRef
{
public:
	int printf(const char *fmt, ...) const noexcept
	{
		char buf[200];
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(buf, sizeof(buf), fmt, vargs);
		va_end(vargs);
		text = buf;
		return ret;
	}

	mutable std::string text;
};

class GCodes
{
public:
	bool LockAllMovementSystemsAndWaitForStandstill(GCodeBuffer&) noexcept { return standstill; }
};

class RepRap
{
public:
	GCodes& GetGCodes() noexcept { return gcodes; }
	void MoveUpdated() noexcept { ++moveUpdates; }

private:
	GCodes gcodes;
};

static RepRap reprap;

// The members of DDARing in src/Movement/DDARing.h that ConfigureMovementQueue uses
class DDARing
{
public:
	GCodeResult ConfigureMovementQueue(GCodeBuffer& gb, const StringRef& reply) THROWS(GCodeException);

	void Init(unsigned int numDdas) noexcept;
	std::string CheckRing(unsigned int expected) const noexcept;

	DDA *addPointer;
	unsigned int numDdasInRing;
	uint32_t gracePeriod = 10;
};

#include "MovementQueue.inc"

// As DDARing::Init1: a ring of numDdas DDAs, addPointer at the first
void DDARing::Init(unsigned int numDdas) noexcept
{
	numDdasInRing = numDdas;
	addPointer = new DDA(nullptr);
	DDA *dda = addPointer;
	for (unsigned int i = 1; i < numDdas; i++)
	{
		DDA * const oldDda = dda;
		dda = new DDA(dda);
		oldDda->SetPrevious(dda);
	}
	addPointer->SetNext(dda);
	dda->SetPrevious(addPointer);
}

// Walk the ring both ways from addPointer and check that it has the expected number of DDAs
std::string DDARing::CheckRing(unsigned int expected) const noexcept
{
	if (numDdasInRing != expected)
	{
		return "numDdasInRing is " + std::to_string(numDdasInRing) + ", expected " + std::to_string(expected);
	}
	unsigned int n = 0;
	const DDA *d = addPointer;
	do
	{
		if (d->GetNext()->GetPrevious() != d)
		{
			return "the ring is broken: the next DDA's previous is not this one";
		}
		d = d->GetNext();
		++n;
	} while (d != addPointer && n <= expected);
	if (n != expected)
	{
		return "the ring has " + std::to_string(n) + " DDAs forwards, expected " + std::to_string(expected);
	}
	return "";
}

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, const std::string& why) noexcept
{
	std::string all = why;
	for (const std::string& f : failures)
	{
		all += (all.empty() ? "" : "; ") + f;
	}
	if (suspended != 0)
	{
		all += (all.empty() ? "" : "; ") + std::string("the scheduler was left suspended");
	}
	if (all.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s: %s\n", name, all.c_str());
	}
	failures.clear();
	suspended = 0;
}

int main()
{
	{
		DDARing ring;
		ring.Init(3);
		DDA *const add = ring.addPointer;
		DDA *const before = add->GetPrevious();
		const size_t allocated = permanent.size();
		GCodeBuffer gb;
		gb.hasP = true;
		gb.p = 6;
		StringRef reply;
		const GCodeResult rslt = ring.ConfigureMovementQueue(gb, reply);
		std::string why = ring.CheckRing(6);
		if (rslt != GCodeResult::ok) { why += " result is not ok"; }
		if (permanent.size() - allocated != 3) { why += " not 3 DDAs allocated"; }
		if (ring.addPointer != add || add->GetPrevious() != before)
		{
			why += " addPointer or the DDA before it changed";
		}
		Report("M595 P6 on a ring of 3 allocates 3 DDAs with the scheduler running and keeps the ring consistent", why);
	}

	{
		DDARing ring;
		ring.Init(5);
		const size_t allocated = permanent.size();
		GCodeBuffer gb;
		gb.hasP = true;
		gb.p = 2;
		StringRef reply;
		(void)ring.ConfigureMovementQueue(gb, reply);
		std::string why = ring.CheckRing(5);
		if (permanent.size() != allocated) { why += " DDAs were allocated"; }
		Report("M595 P2 on a ring of 5 leaves the ring alone", why);
	}

	{
		DDARing ring;
		ring.Init(3);
		const size_t allocated = permanent.size();
		neverUsedRam = 100;
		GCodeBuffer gb;
		gb.hasP = true;
		gb.p = 10;
		StringRef reply;
		const GCodeResult rslt = ring.ConfigureMovementQueue(gb, reply);
		neverUsedRam = 100000;
		std::string why = ring.CheckRing(3);
		if (rslt != GCodeResult::error) { why += " result is not error"; }
		if (reply.text.find("insufficient RAM") != 0) { why += " reply is \"" + reply.text + "\""; }
		if (permanent.size() != allocated) { why += " DDAs were allocated"; }
		Report("M595 with too little RAM gives an error and allocates nothing", why);
	}

	{
		DDARing ring;
		ring.Init(4);
		GCodeBuffer gb;
		StringRef reply;
		(void)ring.ConfigureMovementQueue(gb, reply);
		std::string why = ring.CheckRing(4);
		if (reply.text != "DDAs 4, GracePeriod 10") { why += " reply is \"" + reply.text + "\""; }
		Report("M595 without parameters reports the settings", why);
	}

	for (void *p : permanent)
	{
		free(p);
	}
	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
