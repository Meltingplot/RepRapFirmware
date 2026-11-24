// Binary code fuzz stand-in for GCodes/GCodeMachineState.h: the members that BinaryParser writes (GCodeMachineState.h:247, 266, 291).
// Like the real one it includes ObjectModel/Variable.h, which is where BinaryParser.h gets ExpressionValue and VariableSet from

#ifndef BINARY_CODE_FUZZ_GCODEMACHINESTATE_H
#define BINARY_CODE_FUZZ_GCODEMACHINESTATE_H

#include <BinaryShim.h>
#include <ObjectModel/Variable.h>

class GCodeMachineState final
{
public:
	uint32_t lineNumber = 0;
	bool g53Active = false;
	FilePosition fpos = noFilePosition;
};

#endif
