/*
 * ParseException.h
 *
 *  Created on: 21 Dec 2019
 *      Author: David
 */

#ifndef SRC_GCODES_GCODEEXCEPTION_H_
#define SRC_GCODES_GCODEEXCEPTION_H_

#include <RepRapFirmware.h>
#include <Platform/StringHandle.h>

namespace StackUsage
{
	constexpr uint32_t Throw = 1050;					// how much stack we need to throw an exception
	constexpr uint32_t Margin = 300;					// the margin we allow for calls to non-recursive functions that can throw
}

// This message is used in many places. Define it here to ensure consistency.
constexpr const char *_ecv_array ArrayIndexOutOfRangeText = "array index out of bounds";

// The message of a GCodeException. The exception keeps only a pointer to the message and uses the message as a printf format,
// so the message must be a constant string in flash memory, or null. The constructor is consteval, so a message that is not a
// compile-time constant does not compile. Text that is built at run time, such as a reply buffer, must be passed as the string
// parameter of a "%s" message instead, which the exception copies.
class GCodeExceptionMessage
{
public:
	consteval GCodeExceptionMessage(const char *_ecv_array _ecv_null s) noexcept : text(s) { }

	const char *_ecv_array _ecv_null GetText() const noexcept { return text; }
	bool IsNull() const noexcept { return text == nullptr; }

private:
	const char *_ecv_array _ecv_null const text;
};

// This class is mostly used to throw exceptions when processing GCode. It is also used to store error messages that need to be retrieved later.
// Field "message" always points to a constant string in flash memory, or is null, because it is set only from a GCodeExceptionMessage.
// The error message may have a string, int32_t or uint32_t parameter
class GCodeException
{
public:
	GCodeException() noexcept : line(-1), column(-1), message(nullptr) { }
	explicit GCodeException(GCodeExceptionMessage msg, int32_t iparam = 0) noexcept: line(-1), column(-1), message(msg.GetText()) { param.i = iparam; }

	GCodeException(int lin, int col, GCodeExceptionMessage msg) noexcept : line(lin), column(col), message(msg.GetText()) { }

	GCodeException(int lin, int col, GCodeExceptionMessage msg, const char *_ecv_array sparam) noexcept : line(lin), column(col), message(msg.GetText())
	{
		stringParam.copy(sparam);
	}

	GCodeException(int lin, int col, GCodeExceptionMessage msg, uint32_t uparam) noexcept : line(lin), column(col), message(msg.GetText())
	{
		param.u = uparam;
	}

	GCodeException(int lin, int col, GCodeExceptionMessage msg, int32_t iparam) noexcept : line(lin), column(col), message(msg.GetText())
	{
		param.i = iparam;
	}

	GCodeException(const GCodeBuffer *_ecv_null gb, int col, GCodeExceptionMessage msg) noexcept;

	GCodeException(const GCodeBuffer *_ecv_null gb, int col, GCodeExceptionMessage msg, uint32_t uparam) noexcept;

	GCodeException(const GCodeBuffer *_ecv_null gb, int col, GCodeExceptionMessage msg, const char *_ecv_array sparam) noexcept;

	void GetMessage(const StringRef& reply, const GCodeBuffer *_ecv_null gb) const noexcept;

	void DebugPrint() const noexcept;

	bool IsNull() const noexcept { return message == nullptr; }

private:
	AutoStringHandle file;
	int line;
	int column;
	const char *_ecv_array _ecv_null message;
	union
	{
		int32_t i;
		uint32_t u;
	} param;
	String<StringLength100> stringParam;				// long enough for the reply text from a heater or an expansion board
};

// Functions to create and throw an exception. Using these avoids allocating the GCodeException object on the local stack when it is not going to be used.
[[noreturn]] void __attribute__((noinline)) ThrowGCodeException(GCodeExceptionMessage errMsg) THROWS(GCodeException);
[[noreturn]] void __attribute__((noinline)) ThrowGCodeException(GCodeExceptionMessage errMsg, uint32_t param) THROWS(GCodeException);

#endif /* SRC_GCODES_GCODEEXCEPTION_H_ */
