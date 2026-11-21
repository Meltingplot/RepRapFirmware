// Host stand-in for RepRapFirmware.h: what the real GCodeException.h and GCodeException.cpp need, and what the functions that
// gcode_exception_test.cpp copies out of Heater.cpp, SwitchEndstop.cpp and CanInterface.cpp need from it. The string classes,
// the bitmaps, the length constants and GCodeResult are the real ones from RRFLibraries, src/Config and CANlib.

#ifndef GCODE_EXCEPTION_TEST_REPRAPFIRMWARE_H
#define GCODE_EXCEPTION_TEST_REPRAPFIRMWARE_H

#include <ecv_duet3d.h>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstring>

#define SAME70					1
#define SUPPORT_ASYNC_MOVES		1
#define SUPPORT_CAN_EXPANSION	1
#define HAS_SBC_INTERFACE		0				// the GCodeException constructor then needs neither reprap nor the task list

#define THROWS(...)				noexcept(false)	// as RepRapFirmware.h defines it for the compiler

#include <Config/Configuration.h>
#include <General/String.h>
#include <General/StringRef.h>
#include <General/Bitmap.h>
#include <GCodeResult.h>

typedef Bitmap<uint32_t> LogicalDrivesBitmap;	// RepRapFirmware.h, SAME70 build
typedef uint8_t CanAddress;

class GCodeBuffer;								// RepRapFirmware.h declares it too

extern "C" void debugPrintf(const char *_ecv_array fmt, ...) noexcept __attribute__ ((format (printf, 1, 2)));

#endif
