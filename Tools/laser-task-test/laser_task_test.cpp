// Host test of the deletion of the laser task by Move::Exit (src/Movement/Move.cpp), compiled from the real text of Move::Exit,
// Move::WakeLaserTask and DeleteObject (src/RepRapFirmware.h) that the Makefile extracts.
//
// RepRap::EmergencyStop calls Move::Exit, which deletes the laser task. EmergencyStop runs in the main task
// (TaskPriority::SpinPriority, e.g. M112 in GCodes::DoEmergencyStop when RRF executes the code itself, or an M112 trigger), in the
// SBC task (TaskPriority::SbcPriority, for SbcRequest::EmergencyStop and SbcRequest::Reset, which is how DSF sends M112 and M999,
// and when a connection ends) and in the CAN receiver task in expansion mode.
// A task with a higher priority can run at any instruction of the deletion: while the TaskBase destructor terminates the task,
// before its memory is freed and after. At each of these moments Move::laserTask must already be null, because its readers only
// check the pointer. The test plays two readers at each moment: the Move task starting a move, which calls WakeLaserTask, and a
// second emergency stop from a higher priority task, which runs Move::Exit again.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define _ecv_array
#define _ecv_null
#define _ecv_from

// Move::Exit compiles these parts in on the boards that have a laser task and smart drivers
#define SUPPORT_LASER		1
#define SUPPORT_IOBITS		1
#define HAS_SMART_DRIVERS	1

namespace NotifyIndices
{
	constexpr uint32_t Laser = 3;							// src/AppNotifyIndices.h has FirstAvailableApp + 3, the value does not matter here
}

// The state of one case. It is defined before Move::moveTask, whose destructor uses it at exit
enum class Moment { none, afterTaskDelete, beforeFree, afterFree };		// the moments of the deletion at which another task can run

static std::vector<std::string> failures;
static std::set<const void *> freedTasks;					// the laser tasks that operator delete has freed
static const void *dyingTask = nullptr;				// the laser task, from its termination on
static unsigned int laserTaskFrees = 0;
static bool stepInterruptEnabled = true;
static bool stepInterruptEnabledAtDelete = false;
static Moment preemptAt = Moment::none;
static void (*preemptingTask)(const char *) = nullptr;
static bool preempted = false;
static const char *currentMoment = "";					// while the other task runs: the moment at which it runs
static const char *preemptedAt = "";

// Just what Move::Exit and WakeLaserTask use of TaskBase and Task in ../RRFLibraries/src/RTOSIface/RTOSIface.h. A task that has
// been created and not terminated has a nonzero taskId.
class TaskBase
{
public:
	TaskBase() noexcept : taskId(1) { }						// as after Task::Create, which calls AddToList
	~TaskBase() noexcept { TerminateAndUnlink(); }			// as in RTOSIface.h

	void TerminateAndUnlink() noexcept;
	void Give(uint32_t index) noexcept;
	bool IsRunning() const noexcept { return taskId != 0; }

	static void operator delete(void *p) noexcept;		// to play the moments before and after the memory is freed

	uint32_t taskId;
	uint32_t notifications = 0;
};

template<unsigned int StackWords> class Task : public TaskBase
{
private:
	uint32_t stack[StackWords];
};

// Just what Move::Exit uses of StepTimer, SmartDrivers and DDARing
class StepTimer
{
public:
	static void DisableTimerInterrupt() noexcept { stepInterruptEnabled = false; }
	void CancelCallback() noexcept { }
};

namespace SmartDrivers
{
	void Exit() noexcept { }
}

class DDARing
{
public:
	void Exit() noexcept { }
};

// The members of Move in src/Movement/Move.h that Exit and WakeLaserTask use
class Move
{
public:
	void Exit() noexcept;
	void WakeLaserTask() noexcept;

	static constexpr unsigned int MoveTaskStackWords = 450;
	static Task<MoveTaskStackWords> moveTask;
	static constexpr size_t LaserTaskStackWords = 300;
	static Task<LaserTaskStackWords> *_ecv_null laserTask;

	DDARing rings[2];
	StepTimer stepsTimer;
};

Task<Move::MoveTaskStackWords> Move::moveTask;

#include "LaserTask.inc"

static Move move;

// When a failure happened, for its message
static std::string When() noexcept
{
	return (*currentMoment != 0) ? std::string(currentMoment)
			: (preempted) ? std::string("(the other task ran ") + preemptedAt + ")"
				: std::string();
}

static void ResetState(Moment m, void (*task)(const char *)) noexcept
{
	failures.clear();
	freedTasks.clear();
	dyingTask = nullptr;
	laserTaskFrees = 0;
	stepInterruptEnabled = true;
	stepInterruptEnabledAtDelete = false;
	preemptAt = m;
	preemptingTask = task;
	preempted = false;
	currentMoment = preemptedAt = "";
	Move::moveTask.taskId = 1;
}

static void Preempt(Moment m, const char *moment) noexcept
{
	if (!preempted && m == preemptAt && preemptingTask != nullptr)
	{
		preempted = true;
		currentMoment = preemptedAt = moment;
		preemptingTask(moment);
		currentMoment = "";
	}
}

// As RTOSIface.cpp: if the task is running, clear taskId, unlink the task and vTaskDelete it. The check of freedTasks comes first,
// so that the test reports a destructor that runs on freed memory instead of touching it.
void TaskBase::TerminateAndUnlink() noexcept
{
	if (freedTasks.count(this) != 0)
	{
		failures.push_back(std::string("the destructor of the laser task ran again on its freed memory ") + When());
		return;
	}
	if (taskId != 0)
	{
		taskId = 0;
		if (this != &Move::moveTask)
		{
			dyingTask = this;
			stepInterruptEnabledAtDelete = stepInterruptEnabled;
			Preempt(Moment::afterTaskDelete, "after vTaskDelete, while its destructor runs");
		}
	}
}

// As xTaskNotifyGiveIndexed, which writes the notification into the task control block and puts a task that waits for a
// notification on the ready list (FreeRTOS tasks.c, xTaskGenericNotify), also when that task has been deleted. The comparison
// comes first, so that the test reports a notification of the deleted task instead of touching it.
void TaskBase::Give(uint32_t index) noexcept
{
	(void)index;
	if (this == dyingTask)
	{
		failures.push_back(std::string("WakeLaserTask gave a notification to the deleted laser task ") + When());
		return;
	}
	++notifications;
}

void TaskBase::operator delete(void *p) noexcept
{
	if (p == nullptr)
	{
		return;
	}
	Preempt(Moment::beforeFree, "before its memory is freed");
	if (freedTasks.count(p) != 0)
	{
		failures.push_back(std::string("the laser task was freed a second time ") + When());
		return;
	}
	::operator delete(p);
	freedTasks.insert(p);
	++laserTaskFrees;
	Preempt(Moment::afterFree, "after its memory has been freed");
}

// The Move task starting a move from standstill: DDARing::Spin calls WakeLaserTask
static void MoveTaskStartsMove(const char *) noexcept
{
	move.WakeLaserTask();
}

// A second emergency stop from a higher priority task, e.g. the SBC task (DSF's M112 or the end of a connection) preempting an
// M112 trigger in the main task
static void SecondEmergencyStop(const char *) noexcept
{
	move.Exit();
}

static unsigned int passed = 0, failed = 0;

static void AddFailure(std::string& why, const std::string& f) noexcept
{
	why += (why.empty() ? "" : "; ") + f;
}

static void Report(const char *name, const std::string& why) noexcept
{
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
}

// Create a running laser task, run Move::Exit as an emergency stop does, with 'task' running at moment 'm' of the deletion of the
// laser task, and check that the laser task was deleted once, that no pointer to it remains and that the Move task was terminated
static void Case(const char *name, Moment m, void (*task)(const char *)) noexcept
{
	ResetState(m, task);
	Move::laserTask = new Task<Move::LaserTaskStackWords>;
	move.Exit();

	std::string why;
	for (const std::string& f : failures)
	{
		AddFailure(why, f);
	}
	if (m != Moment::none && !preempted)
	{
		AddFailure(why, "the moment to run the other task never came");
	}
	if (dyingTask == nullptr)
	{
		AddFailure(why, "the laser task was not terminated");
	}
	else if (stepInterruptEnabledAtDelete)
	{
		AddFailure(why, "the step interrupt, which runs ScanningProbeTimerCallback, was still enabled when the laser task was deleted");
	}
	if (laserTaskFrees != 1)
	{
		AddFailure(why, std::string("the laser task was freed ") + std::to_string(laserTaskFrees) + " times");
	}
	if (Move::laserTask != nullptr)
	{
		AddFailure(why, "Move::laserTask still points at the deleted task");
		Move::laserTask = nullptr;
	}
	if (Move::moveTask.IsRunning())
	{
		AddFailure(why, "the Move task was not terminated");
	}
	Report(name, why);
}

int main()
{
	Case("Exit disables the step interrupt, deletes the laser task and clears the pointer", Moment::none, nullptr);

	{
		ResetState(Moment::none, nullptr);
		Move::laserTask = new Task<Move::LaserTaskStackWords>;
		move.WakeLaserTask();
		const bool woken = (Move::laserTask->notifications == 1);
		delete Move::laserTask;
		Move::laserTask = nullptr;
		Report("WakeLaserTask wakes a running laser task", (woken) ? std::string() : std::string("no notification"));
	}

	{
		ResetState(Moment::none, nullptr);
		Move::laserTask = nullptr;
		move.Exit();
		std::string why;
		if (laserTaskFrees != 0)
		{
			AddFailure(why, "a laser task was freed");
		}
		if (Move::moveTask.IsRunning())
		{
			AddFailure(why, "the Move task was not terminated");
		}
		Report("Exit without a laser task", why);
	}

	Case("the Move task starts a move after vTaskDelete of the laser task", Moment::afterTaskDelete, MoveTaskStartsMove);
	Case("the Move task starts a move before the laser task is freed", Moment::beforeFree, MoveTaskStartsMove);
	Case("the Move task starts a move after the laser task is freed", Moment::afterFree, MoveTaskStartsMove);
	Case("a second emergency stop after vTaskDelete of the laser task", Moment::afterTaskDelete, SecondEmergencyStop);
	Case("a second emergency stop before the laser task is freed", Moment::beforeFree, SecondEmergencyStop);
	Case("a second emergency stop after the laser task is freed", Moment::afterFree, SecondEmergencyStop);

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
