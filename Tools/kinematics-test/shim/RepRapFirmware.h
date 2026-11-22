// Host stand-in for src/RepRapFirmware.h: what src/Platform/PublishedPtr.h uses from it

#ifndef KINEMATICS_TEST_REPRAPFIRMWARE_H
#define KINEMATICS_TEST_REPRAPFIRMWARE_H

#include <cstdint>

#define _ecv_array
#define _ecv_null
#define _ecv_from
#define _ecv_not_null(x) (x)

// On the board RRF_ASSERT resets the processor (RepRapFirmware.h:162-163). Here kinematics_test.cpp records the failure.
void AssertFailed(const char *file, int line) noexcept;
#define RRF_ASSERT(_expr)	do { if (!(_expr)) { AssertFailed(__FILE__, __LINE__); } } while (false)

#endif
