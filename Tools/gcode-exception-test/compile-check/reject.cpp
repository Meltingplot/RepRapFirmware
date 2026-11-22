// Compile check of GCodeExceptionMessage, the cases that must not compile: each CASE passes a message that is not a
// compile-time constant. The Makefile compiles each case on its own and expects the error of the consteval constructor.
// See ../README.md.

#include <RepRapFirmware.h>
#include <GCodes/GCodeException.h>

const char *_ecv_array const NotConstexpr = "a const pointer that is not constexpr";
const char *_ecv_array Lookup(bool b) noexcept;

void Reject(bool b, const char *_ecv_array runtimeText)
{
	String<StringLength100> reply;
	(void)reply;
#if CASE == 1		// a pointer known only at run time
	ThrowGCodeException(runtimeText);
#elif CASE == 2		// the text of a reply buffer, as SwitchEndstop::PrimeAxis threw it
	ThrowGCodeException(reply.c_str());
#elif CASE == 3		// the text of a reply buffer, as Heater::SetTemperature threw it
	throw GCodeException(-1, 1, reply.c_str());
#elif CASE == 4		// a const pointer that is not constexpr
	ThrowGCodeException(NotConstexpr);
#elif CASE == 5		// a choice between two literals at run time
	ThrowGCodeException(b ? "one literal" : "another literal");
#elif CASE == 6		// a function that returns a plain pointer
	ThrowGCodeException(Lookup(b), 1u);
#else
# error "unknown CASE"
#endif
}
