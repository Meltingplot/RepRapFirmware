// Variable test stand-in for RepRapFirmware.h: the heap test shim (../heap-test/shim/HeapTestShim.h) plus what
// ObjectModel/Variable.cpp needs from RepRapFirmware.h, Configuration.h and CoreN2G Core.h
#ifndef VARIABLE_TEST_REPRAPFIRMWARE_H
#define VARIABLE_TEST_REPRAPFIRMWARE_H

#include <HeapTestShim.h>
#include <cctype>

constexpr size_t MaxStringExpressionLength = StringLength256;		// Configuration.h

// CoreN2G Core.h
static inline bool isAlpha(char c) noexcept { return isalpha((unsigned char)c) != 0; }
static inline bool isAlnum(char c) noexcept { return isalnum((unsigned char)c) != 0; }

#endif
