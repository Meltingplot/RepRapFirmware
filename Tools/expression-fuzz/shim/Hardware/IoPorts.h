// Expression fuzz stand-in for Hardware/IoPorts.h: the object model of the target has no ports, so AppendPinName is never called
#ifndef EXPRESSION_FUZZ_IOPORTS_H
#define EXPRESSION_FUZZ_IOPORTS_H
#include <ExprShim.h>
class IoPort
{
public:
	void AppendPinName(const StringRef& str) const noexcept { str.cat("fuzzpin"); }
};
#endif
