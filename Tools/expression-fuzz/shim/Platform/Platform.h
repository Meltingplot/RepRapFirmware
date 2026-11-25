// Expression fuzz stand-in for Platform/Platform.h and Storage/MassStorage.h, Storage/FileStore.h: what ExpressionParser.cpp and
// ObjectModel.cpp call. The file functions read from a small fixed set of files, see expression_fuzz.cpp
#ifndef EXPRESSION_FUZZ_PLATFORM_H
#define EXPRESSION_FUZZ_PLATFORM_H
#include <ExprShim.h>

// Platform/UniqueId.h: the object model of the target has no unique ID values, so these are never called
class UniqueId
{
public:
	void AppendCharsToString(const StringRef& str) const noexcept { str.cat("fuzzid"); }
	void AppendCharsToBuffer(OutputBuffer *buf) const noexcept { }
};

class StringHandle;

class Platform
{
public:
	bool SysFileExists(const char *_ecv_array filename) const noexcept;
	bool SysFileExists(const StringHandle& filename) const noexcept;
	void AppendSysDir(const StringRef & path) const noexcept { path.cat("0:/sys/"); }
	void AppendWebDir(const StringRef & path) const noexcept { path.cat("0:/www/"); }
	ReadLockedPointer<const char> GetSysDir() const noexcept { return ReadLockedPointer<const char>(nullptr, "0:/sys/"); }
	ReadLockedPointer<const char> GetWebDir() const noexcept { return ReadLockedPointer<const char>(nullptr, "0:/www/"); }
	void MessageF(MessageType type, const char *_ecv_array fmt, ...) noexcept __attribute__ ((format (printf, 3, 4)));
};
#endif
