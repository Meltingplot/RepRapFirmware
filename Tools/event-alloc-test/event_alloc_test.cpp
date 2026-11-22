// Host test of the allocation of a new event by Event::AddEventV (src/Platform/Event.cpp), compiled from the real text of the Event
// constructor, Event::AddEvent, Event::AddEventV and Event::FinishedProcessing that the Makefile extracts.
//
// Event uses the freelist new and delete of RRFLibraries General/FreelistManager.h (DECLARE_FREELIST_NEW_DELETE in Event.h). When
// the freelist is empty, FreelistManager::Freelist::AllocateItem calls ::operator new, and so malloc. RRF's malloc takes the malloc
// mutex only while the scheduler is running (GetMallocMutex in src/Platform/Tasks.cpp), so a malloc with the scheduler suspended
// runs without it. If a task of the same or a lower priority was preempted inside malloc, both work on the heap at once.
// AddEventV runs in the tasks that raise events: the Heat task (heater faults, LocalHeater.cpp), the main task (driver events in
// Move.cpp, M957 in GCodes3.cpp, filament errors) and the CAN receiver (events from expansion boards, CommandProcessor.cpp).
//
// The test plays TaskCriticalSectionLocker (RRFLibraries RTOSIface.h), which suspends the scheduler, with a counter, and the
// Event allocator with an empty freelist, so that each new event comes from the heap. Every allocation of an Event must happen with
// the scheduler running. It also checks what AddEventV does: the queue order, that a similar event is not queued
// twice, that a released event is freed, and that the event at the head of the queue that is being processed stays there.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#define _ecv_array
#define _ecv_null
#define SUPPORT_CAN_EXPANSION	1

typedef uint8_t CanAddress;									// CoreN2G src/CoreTypes.h

// The values of the NamedEnum EventType in CANlib src/RRF3Common.h, in the same order, because AddEventV compares them
enum class EventType : uint8_t
{
	main_board_power_fail,
	expansion_reconnect, expansion_timeout,
	heater_fault,
	driver_error, filament_error, driver_stall, driver_warning,
	mcu_temperature_warning, overvoltage, undervoltage,
	board_temperature_warning, board_over_temperature
};

static unsigned int suspended = 0;							// uxSchedulerSuspended
static std::vector<std::string> failures;
static std::set<const void *> liveEvents;						// the events that operator new has handed out and delete has not freed
static unsigned int allocations = 0, releases = 0;

// As TaskCriticalSectionLocker in RRFLibraries src/RTOSIface/RTOSIface.h: vTaskSuspendAll in the constructor, xTaskResumeAll in
// the destructor
class TaskCriticalSectionLocker
{
public:
	TaskCriticalSectionLocker() noexcept { ++suspended; }
	~TaskCriticalSectionLocker() noexcept { --suspended; }
};

// Just what the constructor uses of String<50> (RRFLibraries src/General/String.h)
class EventText
{
public:
	void vprintf(const char *_ecv_array fmt, va_list vargs) noexcept { vsnprintf(buf, sizeof(buf), fmt, vargs); }
	const char *c_str() const noexcept { return buf; }

private:
	char buf[50 + 1];
};

// The members of Event in src/Platform/Event.h that the extracted functions use. operator new stands for the freelist new with an
// empty freelist, which calls ::operator new (FreelistManager.h AllocateItem). operator delete stands for the freelist delete, which
// puts the event on the freelist and never frees memory (FreelistManager.h ReleaseItem), so it may run with the scheduler suspended.
class Event
{
public:
	static void *operator new(size_t sz) noexcept;
	static void operator delete(void *p) noexcept;

	static bool AddEvent(EventType et, uint16_t p_param, CanAddress p_ba, uint8_t devNum, const char *_ecv_array format, ...) noexcept;
	static bool AddEventV(EventType et, uint16_t p_param, CanAddress p_ba, uint8_t devNum, const char *_ecv_array format, va_list vargs) noexcept;
	static bool StartProcessing() noexcept;
	static void FinishedProcessing() noexcept;

	// For the test
	static void Reset() noexcept;
	static std::string Queue() noexcept;

private:
	Event(Event *_ecv_null pnext, EventType et, uint16_t p_param, CanAddress p_ba, uint8_t devNum, const char *_ecv_array format, va_list vargs) noexcept;

	Event *_ecv_null next;					// next event in a linked list
	uint16_t param;							// details about the event, e.g. for a heater fault it is the type of the fault
	EventType type;							// what type of event it is
	CanAddress boardAddress;				// which board it came from
	uint8_t deviceNumber;					// which device raised it, e.g. heater number, driver number, trigger number
	volatile bool isBeingProcessed;			// true if this event is being processed, so it must remain at the head of the queue
	EventText text;							// additional info to display to the user

	static Event *_ecv_null eventsPending;	// linked list of events waiting to be processed
	static unsigned int eventsQueued;
	static unsigned int eventsProcessed;
};

#include "Event.inc"

void *Event::operator new(size_t sz) noexcept
{
	if (suspended != 0)
	{
		failures.push_back("an Event was allocated with the scheduler suspended");
	}
	void *const p = malloc(sz);
	liveEvents.insert(p);
	++allocations;
	return p;
}

void Event::operator delete(void *p) noexcept
{
	if (p == nullptr)
	{
		return;
	}
	if (liveEvents.erase(p) == 0)
	{
		failures.push_back("an Event was released that is not allocated");
		return;
	}
	++releases;
	free(p);
}

// As Event::StartProcessing in Event.cpp, without the parts that the test does not need
bool Event::StartProcessing() noexcept
{
	Event *_ecv_null const ev = eventsPending;
	if (ev == nullptr)
	{
		return false;
	}
	ev->isBeingProcessed = true;
	return true;
}

void Event::Reset() noexcept
{
	while (eventsPending != nullptr)
	{
		Event *const ev = eventsPending;
		eventsPending = ev->next;
		delete ev;
	}
	eventsQueued = eventsProcessed = 0;
}

// The queue as text: type number, device number, parameter and text of each event, '*' for an event that is being processed
std::string Event::Queue() noexcept
{
	std::string s;
	for (const Event *ev = eventsPending; ev != nullptr; ev = ev->next)
	{
		char buf[100];
		snprintf(buf, sizeof(buf), "%s%u/%u/%u/%s%s", (s.empty()) ? "" : " ", (unsigned int)ev->type, ev->deviceNumber, ev->param,
					ev->text.c_str(), (ev->isBeingProcessed) ? "*" : "");
		s += buf;
	}
	return s;
}

static unsigned int passed = 0, failed = 0;

static void Expect(std::string& why, bool ok, const std::string& f) noexcept
{
	if (!ok)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
}

static void Report(const char *name, std::string& why, const std::string& expectedQueue) noexcept
{
	const std::string q = Event::Queue();
	Expect(why, q == expectedQueue, "queue is \"" + q + "\", expected \"" + expectedQueue + "\"");
	for (const std::string& f : failures)
	{
		Expect(why, false, f);
	}
	Expect(why, suspended == 0, "the scheduler was left suspended");
	Expect(why, allocations == releases + liveEvents.size(), "allocations and releases do not match");
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s: %s\n", name, why.c_str());
	}
	Event::Reset();
	failures.clear();
	allocations = releases = 0;
	liveEvents.clear();
}

int main()
{
	{
		std::string why;
		Expect(why, Event::AddEvent(EventType::heater_fault, 2, 0, 1, "heater %u", 1u), "AddEvent returned false on an empty queue");
		Report("a new event is allocated with the scheduler running and queued", why, "3/1/2/heater 1");
	}

	{
		std::string why;
		(void)Event::AddEvent(EventType::driver_stall, 0, 0, 4, "a");
		(void)Event::AddEvent(EventType::heater_fault, 1, 0, 0, "b");
		(void)Event::AddEvent(EventType::driver_stall, 0, 0, 5, "c");
		(void)Event::AddEvent(EventType::expansion_timeout, 0, 20, 0, "d");
		Report("events are queued in priority order, the same type in arrival order", why, "2/0/0/d 3/0/1/b 6/4/0/a 6/5/0/c");
	}

	{
		std::string why;
		(void)Event::AddEvent(EventType::driver_error, 7, 0, 2, "first");
		Expect(why, !Event::AddEvent(EventType::driver_error, 7, 0, 2, "second"), "AddEvent returned true for a similar event");
		Expect(why, allocations == 2 && releases == 1, "the similar event was not allocated and released once (allocations "
					+ std::to_string(allocations) + ", releases " + std::to_string(releases) + ")");
		Report("a similar event is released and not queued", why, "4/2/7/first");
	}

	{
		std::string why;
		(void)Event::AddEvent(EventType::driver_error, 7, 10, 2, "board 10");
		Expect(why, Event::AddEvent(EventType::driver_error, 7, 11, 2, "board 11"), "an event from another board was taken as similar");
		Report("an event from another CAN board is not similar", why, "4/2/7/board 10 4/2/7/board 11");
	}

	{
		std::string why;
		(void)Event::AddEvent(EventType::driver_warning, 0, 0, 0, "processing");
		Expect(why, Event::StartProcessing(), "StartProcessing found no event");
		(void)Event::AddEvent(EventType::heater_fault, 0, 0, 0, "urgent");
		Expect(why, !Event::AddEvent(EventType::driver_warning, 0, 0, 0, "again"), "an event similar to the one being processed was queued");
		Report("the event being processed stays at the head and suppresses similar ones", why, "7/0/0/processing* 3/0/0/urgent");
	}

	{
		std::string why;
		(void)Event::AddEvent(EventType::heater_fault, 0, 0, 0, "x");
		(void)Event::StartProcessing();
		Event::FinishedProcessing();
		Expect(why, releases == 1, "FinishedProcessing did not release the event");
		Report("FinishedProcessing releases the processed event", why, "");
	}

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
