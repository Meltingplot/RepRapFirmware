// Binary code fuzz stand-in for Networking/NetworkDefs.h: MacAddress as far as BinaryParser::GetMacAddress writes it (NetworkDefs.h:83)

#ifndef BINARY_CODE_FUZZ_NETWORKDEFS_H
#define BINARY_CODE_FUZZ_NETWORKDEFS_H

#include <BinaryShim.h>

struct MacAddress
{
	uint8_t bytes[6];
};

#endif
