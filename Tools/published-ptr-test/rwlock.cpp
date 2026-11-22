// The ReadWriteLock of RRFLibraries, compiled from the real text of src/RTOSIface/RTOSIface.cpp that the Makefile extracts into
// gen/ReadWriteLock.inc, with the real RTOSIface.h and General/FreelistManager.h, on the host RTOS of shim/HostRtos.h

#include <RepRapFirmware.h>											// the stand-in, for TestAssertFailed
#include <RTOSIface/RTOSIface.h>
#include <RTOSIface/RTOSNotifyIndices.h>
#include <General/FreelistManager.h>

// As at the top of RTOSIface.cpp (:19-20), but the failure is recorded instead of resetting the processor
#ifdef PUBLISHED_PTR_TEST_NO_CHECKS
# define RTOS_ASSERT(_expr)	(void)sizeof(_expr);
#else
# define RTOS_ASSERT(_expr)	if (!(_expr)) { TestAssertFailed(__LINE__, __FILE__); }
#endif

#include "ReadWriteLock.inc"

// Called by ~TaskBase. The test's tasks are never in the task list of RTOSIface.cpp.
void TaskBase::TerminateAndUnlink() noexcept
{
}

// End
