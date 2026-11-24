// Expression fuzz stand-in for Networking/NetworkDefs.h: MacAddress as declared there (NetworkDefs.h:83-96); the object model of the
// target has one MAC address, see expression_fuzz.cpp
#ifndef EXPRESSION_FUZZ_NETWORKDEFS_H
#define EXPRESSION_FUZZ_NETWORKDEFS_H
#include <ExprShim.h>
struct MacAddress
{
	uint8_t bytes[6];
	uint32_t LowWord() const noexcept { return (((((bytes[3] << 8) | bytes[2]) << 8) | bytes[1]) << 8) | bytes[0]; }
	uint16_t HighWord() const noexcept { return (bytes[5] << 8) | bytes[4]; }
};
#endif
