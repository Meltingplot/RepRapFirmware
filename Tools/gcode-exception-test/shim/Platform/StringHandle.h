// Host stand-in for Platform/StringHandle.h. GCodeException keeps the name of the file that threw in an AutoStringHandle. The
// heater and endstop code throws without a GCodeBuffer, so the name is never set and the handle only has to be a null one.

#ifndef GCODE_EXCEPTION_TEST_STRINGHANDLE_H
#define GCODE_EXCEPTION_TEST_STRINGHANDLE_H

class AutoStringHandle
{
public:
	struct Text
	{
		const char *Ptr() const noexcept { return ""; }
	};

	bool IsNull() const noexcept { return true; }
	Text Get() const noexcept { return Text(); }
};

#endif
