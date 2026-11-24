// Expression fuzz stand-in for Hardware/ExceptionHandlers.h: SoftwareReset resets the board; the stand-in reports it, see expression_fuzz.cpp
#ifndef EXPRESSION_FUZZ_EXCEPTIONHANDLERS_H
#define EXPRESSION_FUZZ_EXCEPTIONHANDLERS_H
#include <ExprShim.h>
enum class SoftwareResetReason : uint16_t { stackOverflow = 0x0C0 };
[[noreturn]] void SoftwareReset(SoftwareResetReason initialReason, const uint32_t *_ecv_array _ecv_null stk = nullptr) noexcept;
#endif
