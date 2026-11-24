// Binary code fuzz stand-in for CoreN2G CoreTypes.h, for CANlib CanId.h: the CAN address type only (CoreTypes.h:26). The real header
// declares a __fp16 type, which g++ supports on ARM only
#ifndef BINARY_CODE_FUZZ_CORETYPES_H
#define BINARY_CODE_FUZZ_CORETYPES_H
#include <cstdint>
typedef uint8_t CanAddress;
#endif
