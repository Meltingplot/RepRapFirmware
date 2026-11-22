// The Mutex and ReadWriteLock members of RRFLibraries src/RTOSIface/RTOSIface.cpp, compiled from the real text that the Makefile
// extracts into gen/ (RTOSIface-mutex.inc: lines 19-69, the vAssertCalled declaration, RTOS_ASSERT and the Mutex members; the ReadWriteLock
// part from "Class ReadWriteLock members" to "End"), with the real RTOSIface.h and General/FreelistManager.h, on the host RTOS of shim/.
// The RRFLibraries tree is the one that the Makefile's RRFLIB names.

#include <RTOSIface/RTOSIface.h>
#include <RTOSIface/RTOSNotifyIndices.h>
#include <General/FreelistManager.h>

#include "gen/RTOSIface-mutex.inc"
#include "gen/RTOSIface-rwlock.inc"

// Called by ~TaskBase. The test's tasks are never in the task list of RTOSIface.cpp.
void TaskBase::TerminateAndUnlink() noexcept
{
}

// End
