// Expression fuzz stand-in for Platform/OutputMemory.h: OutputBuffer with the members that ObjectModel.cpp and Variable.cpp call
// (OutputMemory.h:37-64). Only the JSON report functions of ObjectModel.cpp use it, and the target does not call them; the stand-in
// collects the text in a String so that a call would still work
#ifndef EXPRESSION_FUZZ_OUTPUTMEMORY_H
#define EXPRESSION_FUZZ_OUTPUTMEMORY_H
#include <ExprShim.h>
class OutputBuffer
{
public:
	size_t Length() const noexcept { return text.strlen(); }
	size_t catf(const char *_ecv_array fmt, ...) noexcept __attribute__ ((format (printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const size_t n = text.GetRef().vcatf(fmt, vargs);
		va_end(vargs);
		return n;
	}
	size_t cat(const char c) noexcept { text.cat(c); return 1; }
	size_t cat(const char *_ecv_array src) noexcept { text.cat(src); return strlen(src); }
	size_t cat(const char *_ecv_array src, size_t len) noexcept { text.catn(src, len); return len; }
	size_t cat(StringRef &str) noexcept { text.cat(str.c_str()); return str.strlen(); }
	size_t EncodeChar(char c) noexcept { text.cat(c); return 1; }
	bool HadOverflow() const noexcept { return false; }			// the stand-in never runs out of buffers
private:
	String<StringLength256> text;
};
#endif
