// Stand-in for src/RepRapFirmware.h, as far as src/GCodes/TriggerItem.h and the extracted functions use it. The types are those of the
// Duet 3 MB6HC, which supports CAN expansion (src/RepRapFirmware.h:408-429). The sizes come from gen/Sizes.inc, see the Makefile.

#ifndef TRIGGER_ACTION_TEST_REPRAPFIRMWARE_H
#define TRIGGER_ACTION_TEST_REPRAPFIRMWARE_H

// The standard headers first: General/Bitmap.h brings in the eCv keyword macros (RRFLibraries src/ecv_original.h)
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <General/Bitmap.h>							// RRFLibraries, the real one

#define THROWS(...)

#include "Sizes.inc"								// MaxTriggers of the MB6HC and MaxGpInPorts of Duet 3

typedef Bitmap<uint32_t> AxesBitmap;				// src/RepRapFirmware.h:410
typedef Bitmap<uint64_t> InputPortsBitmap;			// src/RepRapFirmware.h:413
typedef Bitmap<uint32_t> TriggerNumbersBitmap;		// src/RepRapFirmware.h:429

static_assert(MaxGpInPorts <= InputPortsBitmap::MaxBits());
static_assert(MaxTriggers <= TriggerNumbersBitmap::MaxBits());

// src/Config/Configuration.h:147-158
constexpr size_t StringLength20 = 20;
constexpr size_t StringLength256 = 256;
constexpr size_t StringLength500 = 500;
constexpr size_t GCodeReplyLength = StringLength500;

// CANlib src/GCodeResult.h
enum class GCodeResult : uint8_t
{
	notFinished, ok, warning, warningNotSupported, error, errorNotSupported, notSupportedInCurrentMode, stopped, badOrMissingParameter,
	remoteInternalError, m291Cancelled, waitingForAckSoIgnored, noCanBuffer, canResponseTimeout, unknownCanBoard
};

// src/Platform/MessageType.h, only the two names that the extracted functions use. The values do not matter here.
enum MessageType : uint32_t { GenericMessage = 1, ErrorMessage = 2 };

class GCodeBuffer;

// What the extracted functions use of StringRef and String (RRFLibraries src/General/StringRef.h and String.h)
class StringRef
{
public:
	StringRef(char *_ecv_array pp, size_t plen) noexcept : p(pp), len(plen) { }
	const char *_ecv_array c_str() const noexcept { return p; }
	size_t strlen() const noexcept { return std::strlen(p); }
	void Clear() const noexcept { p[0] = 0; }
	int copy(const char *_ecv_array s) const noexcept { return std::snprintf(p, len, "%s", s); }
	int cat(const char *_ecv_array s) const noexcept { return catf("%s", s); }
	int printf(const char *_ecv_array fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list args;
		va_start(args, fmt);
		const int ret = std::vsnprintf(p, len, fmt, args);
		va_end(args);
		return ret;
	}
	int catf(const char *_ecv_array fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		const size_t n = std::strlen(p);
		va_list args;
		va_start(args, fmt);
		const int ret = std::vsnprintf(p + n, len - n, fmt, args);
		va_end(args);
		return ret;
	}

private:
	char *_ecv_array p;
	size_t len;
};

template<size_t N> class String
{
public:
	String() noexcept { buf[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(buf, N + 1); }
	const char *_ecv_array c_str() const noexcept { return buf; }
	char operator[](size_t index) const noexcept { return buf[index]; }
	size_t strlen() const noexcept { return std::strlen(buf); }
	void Clear() noexcept { buf[0] = 0; }
	void copy(const char *_ecv_array s) noexcept { GetRef().copy(s); }
	void cat(const char *_ecv_array s) noexcept { GetRef().cat(s); }
	int printf(const char *_ecv_array fmt, ...) noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list args;
		va_start(args, fmt);
		const int ret = std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		return ret;
	}
	int catf(const char *_ecv_array fmt, ...) noexcept __attribute__((format(printf, 2, 3)))
	{
		const size_t n = std::strlen(buf);
		va_list args;
		va_start(args, fmt);
		const int ret = std::vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
		va_end(args);
		return ret;
	}

private:
	char buf[N + 1];
};

// GCodeException (src/GCodes/GCodeException.h) with a ready message
class GCodeException
{
public:
	GCodeException(const GCodeBuffer *_ecv_null gb, int col, const char *_ecv_array msg) noexcept : message(msg) { }
	void GetMessage(const StringRef& reply, const GCodeBuffer *_ecv_null gb) const noexcept { reply.copy(message.c_str()); }

private:
	std::string message;
};

// RRFLibraries src/General/StringFunctions.h and SafeStrtod.h, and isDigit
inline bool StringStartsWith(const char *_ecv_array s, const char *_ecv_array starting) noexcept
{
	return std::strncmp(s, starting, std::strlen(starting)) == 0;
}

inline bool StringStartsWithIgnoreCase(const char *_ecv_array s, const char *_ecv_array starting) noexcept
{
	return strncasecmp(s, starting, std::strlen(starting)) == 0;
}

inline bool isDigit(char c) noexcept { return isdigit((int)c) != 0; }
inline bool isAlnum(char c) noexcept { return isalnum((int)c) != 0; }

inline uint32_t StrToU32(const char *_ecv_array s, const char *_ecv_array *_ecv_null endptr = nullptr) noexcept
{
	char *end;
	const uint32_t ret = (uint32_t)std::strtoul(s, &end, 10);
	if (endptr != nullptr) { *endptr = end; }
	return ret;
}

uint32_t millis() noexcept;							// the clock of the test

#endif
