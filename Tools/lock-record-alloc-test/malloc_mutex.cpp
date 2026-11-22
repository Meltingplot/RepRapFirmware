// GetMallocMutex and ReleaseMallocMutex, the MALLOC_LOCK and MALLOC_UNLOCK of nano-mallocr.c (:131-135), compiled from the real text
// of RRF src/Platform/Tasks.cpp ("// Mutexes" to the end of ReleaseMallocMutex, :108-128) that the Makefile extracts into gen/.

#include <RTOSIface/RTOSIface.h>

#include "gen/Tasks-malloc-mutex.inc"

// As AppMain does before it starts the scheduler (Tasks.cpp:281)
void TestCreateMallocMutex() noexcept
{
	mallocMutex.Create("Malloc");
}

TaskHandle TestMallocMutexHolder() noexcept
{
	return mallocMutex.GetHolder();
}

// End
