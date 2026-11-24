// Expression fuzz stand-in for PrintMonitor/PrintMonitor.h: the custom info of the job file (PrintMonitor.h:69)
#ifndef EXPRESSION_FUZZ_PRINTMONITOR_H
#define EXPRESSION_FUZZ_PRINTMONITOR_H
#include <ExprShim.h>
#include <ObjectModel/Variable.h>
class PrintMonitor
{
public:
	ReadLockedPointer<const VariableSet> GetCustomInfoForReading() noexcept { return ReadLockedPointer<const VariableSet>(customInfoLock, &customInfo); }
	ReadWriteLock customInfoLock;
	VariableSet customInfo;
};
#endif
