// Binary code fuzz stand-in for GCodes/GCodeBuffer/ExpressionParser.h, found by the "ExpressionParser.h" include of the copy of
// BinaryParser.cpp in gen/. The real parser reads the expression through CurrentCharacter(), which returns *currentp only while
// currentp < endp (ExpressionParser.cpp:2608), and Parse starts with SkipWhiteSpace, which reads the first character. This stand-in
// makes that first read and nothing more, so that an expression whose start BinaryParser computed wrongly is read once, as the real
// parser would; it then returns a value chosen by that character instead of evaluating anything:
//   digit: that number   '-': -1   'f': 1.5   '"': the literal string "fuzz"   'h': a heap string "fuzz"   else: GCodeException

#ifndef BINARY_CODE_FUZZ_EXPRESSIONPARSER_H
#define BINARY_CODE_FUZZ_EXPRESSIONPARSER_H

#include <BinaryShim.h>
#include <GCodes/GCodeException.h>
#include <ObjectModel/ObjectModel.h>

class ExpressionParser
{
public:
	ExpressionParser(const GCodeBuffer *_ecv_null p_gb, const char *_ecv_array text, const char *_ecv_array textLimit, int p_column = -1) noexcept
		: currentp(text), endp(textLimit) { (void)p_gb; (void)p_column; }

	ExpressionValue Parse(bool evaluate = true) THROWS(GCodeException);
	float ParseFloat() THROWS(GCodeException);
	int32_t ParseInteger() THROWS(GCodeException);
	uint32_t ParseUnsigned() THROWS(GCodeException);
	DriverId ParseDriverId() THROWS(GCodeException);
	void ParseFloatArray(float arr[], size_t& length) THROWS(GCodeException);
	void ParseIntArray(int32_t arr[], size_t& length) THROWS(GCodeException);
	void ParseUnsignedArray(uint32_t arr[], size_t& length) THROWS(GCodeException);
	void ParseDriverIdArray(DriverId arr[], size_t& length) THROWS(GCodeException);
	void CheckForExtraCharacters() THROWS(GCodeException) { }

private:
	char CurrentCharacter() const noexcept { return (currentp < endp) ? *currentp : 0; }		// ExpressionParser.cpp:2608
	int32_t Number() THROWS(GCodeException);

	const char *_ecv_array currentp;
	const char *_ecv_array endp;
};

#endif
