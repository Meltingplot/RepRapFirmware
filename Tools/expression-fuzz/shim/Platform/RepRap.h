// Expression fuzz stand-in for Platform/RepRap.h: the root of the object model and the global variables (RepRap.h:172). The object
// model below the root is played by small classes in expression_fuzz.cpp that use the real table macros of ObjectModel.h
#ifndef EXPRESSION_FUZZ_REPRAP_H
#define EXPRESSION_FUZZ_REPRAP_H
#include <ExprShim.h>
#include <ObjectModel/ObjectModel.h>
#include <ObjectModel/Variable.h>
#include <Platform/Platform.h>
#include <PrintMonitor/PrintMonitor.h>

class RepRap INHERIT_OBJECT_MODEL
{
public:
	Platform& GetPlatform() noexcept { return platform; }
	PrintMonitor& GetPrintMonitor() noexcept { return printMonitor; }
	ReadLockedPointer<const VariableSet> GetGlobalVariablesForReading() noexcept { return ReadLockedPointer<const VariableSet>(globalsLock, &globals); }
	bool UsingSbcInterface() const noexcept { return usingSbc; }
	DebugFlags GetDebugFlags(Module) const noexcept { return debugFlags; }

	Platform platform;
	PrintMonitor printMonitor;
	ReadWriteLock globalsLock;
	VariableSet globals;
	DebugFlags debugFlags;
	bool usingSbc = true;

protected:
	DECLARE_OBJECT_MODEL_WITH_ARRAYS
};
extern RepRap reprap;
#endif
