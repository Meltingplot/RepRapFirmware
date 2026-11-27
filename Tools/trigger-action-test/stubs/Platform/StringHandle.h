// Stand-in for src/Platform/StringHandle.h: AutoStringHandle without the heap. As in the real StringHandle::Assign
// (src/Platform/StringHandle.cpp:51-63), assigning an empty string leaves the handle null.

#ifndef TRIGGER_ACTION_TEST_STRINGHANDLE_H
#define TRIGGER_ACTION_TEST_STRINGHANDLE_H

#include <RepRapFirmware.h>

// What Get() returns (RRFLibraries ReadLockedPointer<const char>), as far as the extracted functions use it
class LockedString
{
public:
	explicit LockedString(const char *_ecv_array _ecv_null p) noexcept : ptr(p) { }
	const char *_ecv_array _ecv_null Ptr() const noexcept { return ptr; }

private:
	const char *_ecv_array _ecv_null ptr;
};

class AutoStringHandle
{
public:
	LockedString Get() const noexcept { return LockedString((isNull) ? nullptr : text.c_str()); }
	void Delete() noexcept { text.clear(); isNull = true; }
	bool IsNull() const noexcept { return isNull; }
	void Assign(const char *_ecv_array _ecv_null s) noexcept
	{
		Delete();
		if (s != nullptr && *s != 0)
		{
			text = s;
			isNull = false;
		}
	}

private:
	std::string text;
	bool isNull = true;
};

#endif
