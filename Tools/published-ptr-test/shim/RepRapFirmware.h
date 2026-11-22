// Host stand-in for src/RepRapFirmware.h: what src/Platform/PublishedPtr.h and PublishedPtr.cpp use from it and from CoreN2G

#ifndef PUBLISHED_PTR_TEST_REPRAPFIRMWARE_H
#define PUBLISHED_PTR_TEST_REPRAPFIRMWARE_H

#include <ecv_duet3d.h>												// as RepRapFirmware.h:25
#include <cstdint>
#include <cstddef>
#include "HostRtos.h"

// On the board RRF_ASSERT calls vAssertCalled (RepRapFirmware.h:162-163), which resets the processor. Here the failure is recorded and the
// code goes on. PUBLISHED_PTR_TEST_NO_CHECKS takes the assertions out, to show that the cases which depend on them fail without them.
void TestAssertFailed(uint32_t line, const char *file) noexcept;
#ifdef PUBLISHED_PTR_TEST_NO_CHECKS
# define RRF_ASSERT(_expr)	do { (void)sizeof(_expr); } while (false)
#else
# define RRF_ASSERT(_expr)	do { if (!(_expr)) { TestAssertFailed(__LINE__, __FILE__); } } while (false)
#endif

// CoreN2G Core.h:474 and CMSIS, for the calling thread
inline bool inInterrupt() noexcept { return HostRtos::InInterrupt(); }
inline uint32_t __get_BASEPRI() noexcept { return HostRtos::GetBasePri(); }
inline uint32_t __get_PRIMASK() noexcept { return HostRtos::GetPriMask(); }

#endif
