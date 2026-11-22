// Compile check of GCodeExceptionMessage, the cases that must compile: messages that are compile-time constants, and text that
// is built at run time passed as the string parameter of a "%s" message. See ../README.md.

#include <RepRapFirmware.h>
#include <GCodes/GCodeException.h>

constexpr const char *_ecv_array NamedMessage = "a named constant";

// A function may return a constant message, or a null one for "no error", as SmartDrivers::CheckStallDetectionEnabled does
static GCodeExceptionMessage CheckSomething(bool ok) noexcept
{
	if (ok)
	{
		return nullptr;
	}
	return "driver %u is not in spreadCycle mode";
}

void Accept(bool b, const char *_ecv_array runtimeText)
{
	if (b) { ThrowGCodeException("a literal"); }
	if (b) { ThrowGCodeException("a literal with a parameter %u", 3u); }
	if (b) { ThrowGCodeException(NamedMessage); }
	if (b) { throw GCodeException(-1, -1, "a literal"); }
	if (b) { throw GCodeException(-1, -1, "%s", runtimeText); }
	String<StringLength100> reply;
	if (b) { throw GCodeException(-1, -1, "%s", reply.c_str()); }
	const GCodeExceptionMessage msg = CheckSomething(b);
	if (!msg.IsNull()) { ThrowGCodeException(msg, 1u); }
	GCodeException none;
	(void)none;
}
