// Variable test stand-in for GCodes/GCodeException.h: the message only
#ifndef VARIABLE_TEST_GCODEEXCEPTION_H
#define VARIABLE_TEST_GCODEEXCEPTION_H

#include <RepRapFirmware.h>

class GCodeException
{
public:
	explicit GCodeException(const char *msg) noexcept : message(msg) { }
	const char *GetMessage() const noexcept { return message; }

private:
	const char *message;
};

constexpr const char *_ecv_array ArrayIndexOutOfRangeText = "array index out of bounds";

#endif
