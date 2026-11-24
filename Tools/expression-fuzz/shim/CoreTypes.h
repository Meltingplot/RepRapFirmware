// Expression fuzz stand-in for CoreN2G CoreTypes.h, for CANlib CanId.h: the CAN address type only (CoreTypes.h:26). The real header
// declares a __fp16 type, which g++ supports on ARM only
#ifndef EXPRESSION_FUZZ_CORETYPES_H
#define EXPRESSION_FUZZ_CORETYPES_H
#include <cstdint>
typedef uint8_t CanAddress;
#endif
