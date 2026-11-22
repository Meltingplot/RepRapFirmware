/*
 * PublishedPtr.cpp
 *
 *  Created on: 21 Nov 2026
 *      Author: Tim Schneider
 */

#include "PublishedPtr.h"

// Check rule R3: the caller may read without the lock only if the writer can't run until the caller has finished with the object.
// The step ISR, the Move task and the main task read often, so check the cheap cases first.
void PublishedPtrBase::CheckReader() const noexcept
{
	if (inInterrupt() || RTOSIface::GetCurrentTask() == writer)
	{
		return;
	}

	// No task switch can happen while interrupts are masked or the scheduler is suspended, e.g. under a BasePriorityBooster or a TaskCriticalSectionLocker.
	// Don't ask for the priorities then: uxTaskPriorityGet and uxTaskBasePriorityGet enter and leave a critical section, and leaving it sets BASEPRI to 0.
	if (__get_BASEPRI() != 0 || __get_PRIMASK() != 0 || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
	{
		return;
	}

	// Compare base priorities, because an inherited priority lasts only while a mutex is held. A reader that has inherited a higher priority must still
	// hold the lock, and readers that outrank the writer don't fail while it has inherited a higher priority from a mutex.
	if (uxTaskBasePriorityGet(nullptr) <= uxTaskBasePriorityGet(writer->GetFreeRTOSHandle()))
	{
		lock.CheckHasReadOrWriteLock();
	}
}

// End
