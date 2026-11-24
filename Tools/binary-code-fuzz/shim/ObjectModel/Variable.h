// Binary code fuzz stand-in for ObjectModel/Variable.h: the VariableSet that BinaryParser::AddParameters fills with the parameters of a
// macro call (GCodes.cpp:3718, DoFileMacroWithParameters). It keeps a copy of each value, as the real InsertNew does, and the fuzz
// target reads each one back the way a macro reads param.X (see binary_code_fuzz.cpp)

#ifndef BINARY_CODE_FUZZ_VARIABLE_H
#define BINARY_CODE_FUZZ_VARIABLE_H

#include <BinaryShim.h>
#include <GCodes/GCodeException.h>
#include <ObjectModel/ObjectModel.h>

class VariableSet
{
public:
	static constexpr unsigned int MaxValues = 64;

	void InsertNewParameter(const char *_ecv_array str, ExpressionValue pVal) THROWS(GCodeException);

	unsigned int count = 0;
	char names[MaxValues][2];
	ExpressionValue values[MaxValues];
};

#endif
