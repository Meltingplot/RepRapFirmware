// Host test of Move::SetKinematics (src/Movement/Move.cpp), compiled from the real function text that the Makefile extracts.
//
// The object model reads the kinematics pointer from another task: move.kinematics is OBJECT_MODEL_FUNC(self->kinematics) in
// Move.cpp, and the SBC task (TaskPriority::SbcPriority) serves object model requests while the main task
// (TaskPriority::SpinPriority) runs M665 or M669, which call SetKinematics when the type changes. The SBC task can preempt the
// main task at any instruction, so at every moment of the change move.kinematics must name an intact object: the old one until
// its deletion starts, then the new one. The test plays that task while the old kinematics is destroyed, before its memory is
// freed and after. Without an SBC the network task serves the object model at the main task's priority with time slicing; this
// order does not protect that reader (Tools/pointer-check/census.md).

#include <cstdio>
#include <new>
#include <string>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from
#define _ecv_not_null(x) (x)

// Just what SetKinematics uses
class Kinematics
{
public:
	virtual ~Kinematics() noexcept = default;
	static Kinematics *_ecv_null Create(const char *_ecv_array _ecv_null name, int legacyType) noexcept;
};

class Move
{
public:
	bool SetKinematics(const char *_ecv_array _ecv_null name, int legacyType) noexcept;

	Kinematics *_ecv_from kinematics = nullptr;						// as in Move.h
};

struct RepRap
{
	void MoveUpdated() noexcept;
};

static Move move;
static RepRap reprap;

#include "SetKinematics.inc"

static std::vector<std::string> failures;
static const void *beingDeleted = nullptr;							// the old kinematics, from the start of its destructor on
static unsigned int deletions = 0;

// The other task reading move.kinematics, as the object model does. It only compares the pointer, so it never dereferences
// freed memory itself
static void ObjectModelReads(const char *moment) noexcept
{
	if (beingDeleted != nullptr && move.kinematics == beingDeleted)
	{
		failures.push_back(std::string("the object model reaches the old kinematics ") + moment);
	}
}

class TestKinematics : public Kinematics
{
public:
	~TestKinematics() noexcept override
	{
		beingDeleted = this;
		ObjectModelReads("while its destructor runs");
	}

	static void operator delete(void *p) noexcept
	{
		ObjectModelReads("before its memory is freed");
		::operator delete(p);
		++deletions;
		ObjectModelReads("after its memory has been freed");
		beingDeleted = nullptr;
	}
};

static bool createFails = false;
static Kinematics *_ecv_null created = nullptr;

Kinematics *Kinematics::Create(const char *, int) noexcept
{
	created = createFails ? nullptr : new TestKinematics;
	return created;
}

void RepRap::MoveUpdated() noexcept
{
	// DSF queries move again when its sequence number changes, and then it must get the new kinematics
	if (move.kinematics != created)
	{
		failures.push_back("MoveUpdated runs before move.kinematics names the new kinematics");
	}
}

static unsigned int failed = 0;

static void Report(const char *name, bool resultOk) noexcept
{
	if (resultOk && failures.empty())
	{
		std::printf("PASS  %s\n", name);
		return;
	}
	++failed;
	std::printf("FAIL  %s\n", name);
	if (!resultOk)
	{
		std::printf("        wrong return value, kinematics or number of deletions\n");
	}
	for (const std::string& f : failures)
	{
		std::printf("        %s\n", f.c_str());
	}
}

int main()
{
	// A type change, e.g. M669 K5 in config.g after the default Cartesian kinematics
	failures.clear();
	deletions = 0;
	createFails = false;
	move.kinematics = new TestKinematics;
	bool ok = move.SetKinematics("coreXYU", 5);
	Report("type change: the object model never reaches the old kinematics once its deletion has started",
			ok && move.kinematics == created && deletions == 1);

	// An unknown type leaves the kinematics alone
	failures.clear();
	deletions = 0;
	createFails = true;
	Kinematics *_ecv_from const before = move.kinematics;
	ok = move.SetKinematics("unknown", 99);
	Report("unknown type: the kinematics stays and nothing is deleted", !ok && move.kinematics == before && deletions == 0);

	delete move.kinematics;
	std::printf("%u of 2 failed\n", failed);
	return (failed == 0) ? 0 : 1;
}
