// Fuzz target for the RRF expression parser: an expression as a user writes it in a G-code file, a macro, a USB or PanelDue command,
// or as DSF sends it in SBC mode (EvaluateExpression packet, expression parameters of binary codes), evaluated by the real
// ExpressionParser with the real ExpressionValue, ObjectModel, Variable, Heap, StringHandle and ArrayHandle code.
//
// Real code, compiled unchanged (copies in gen/ only where a quoted include has to find a stand-in, see the Makefile):
// - src/GCodes/GCodeBuffer/ExpressionParser.cpp, src/ObjectModel/ObjectModel.cpp, src/ObjectModel/Variable.cpp,
//   src/Platform/Heap.cpp, src/Platform/StringHandle.cpp, src/Platform/ArrayHandle.cpp, src/libc/strptime.cpp (SafeStrptime)
// - ObjectModel.h with one change for the 64-bit host: the member 'whole' of ExpressionValue is uintptr_t instead of uint32_t, because
//   the union holds pointers (8 bytes on the host, 4 on the board) and ObjectModel.h:70-73 asserts that 'whole' is as big as them
// - GCodeException::GetMessage, ThrowGCodeException, struct DriverId and GetFloatFormatString, copied out at build time
//   (../sbc-packet-fuzz/extract.awk)
// - RRFLibraries StringRef, SafeVsnprintf, Strnlen, StringFunctions, SafeStrtod, NumericConverter, NamedEnum, IPAddress, IP4String
//
// Played here and in shim/ (README.md has the list): the G-code buffer, the object model below the root (small classes that use the
// real table macros of ObjectModel.h and real value types), the files of fileexists() and fileread(), the RTOS locks (single task,
// with deadlock detection), the stack of the board task (board_stack.cpp), random32, millis64, SoftwareReset, ReportInternalError.
//
// Input: byte 0 selects the entry point (see Entry below), byte 1 the array length given to the array parsers (1 to 32) and bit 7
// of it whether the parser gets the G-code buffer (as from StringParser and SbcInterface) or none. The rest is the expression text,
// in a heap block of exactly its length, as the parser gets it from StringParser.cpp:759 (text, end pointer) without a null after it.

#include <cstdlib>
#include <vector>
#include <string>
#include <typeinfo>

#include <ExprShim.h>
#include <GCodeBuffer.h>
#include <GCodes/GCodeBuffer/ExpressionParser.h>
#include <ObjectModel/Variable.h>
#include <Platform/RepRap.h>
#include <Platform/Platform.h>
#include <Networking/NetworkDefs.h>
#include <Hardware/ExceptionHandlers.h>
#include <GCodes/GCodeException.h>
#if defined(__SANITIZE_ADDRESS__)
# define FUZZ_ASAN	1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define FUZZ_ASAN	1
# endif
#endif
#ifdef FUZZ_ASAN
# include <sanitizer/asan_interface.h>
# include <sanitizer/common_interface_defs.h>
#endif

namespace FuzzStack
{
	void Begin(size_t bytes) noexcept;
	const char *StackPointer() noexcept;
	const char *StackLimit() noexcept;
	void *TaskControlBlock() noexcept;
	size_t Depth() noexcept;
	void SetDepth(size_t d) noexcept;
	size_t MaxDepth() noexcept;
	size_t NumMatched() noexcept;
	void CheckThrow() noexcept;
}

namespace
{
	bool verbose = false;
	size_t stackBytes = 4096;

	[[noreturn]] void FuzzReport(const char *what, const char *detail) noexcept
	{
		fprintf(stderr, "ERROR: FUZZ: %s: %s\n", what, detail);
#ifdef FUZZ_ASAN
		__sanitizer_print_stack_trace();
#endif
		fprintf(stderr, "SUMMARY: FUZZ: %s\n", what);
		abort();
	}
}

RepRap reprap;

extern "C" void debugPrintf(const char *fmt, ...) noexcept
{
	if (!verbose) { return; }
	va_list vargs;
	va_start(vargs, fmt);
	vfprintf(stderr, fmt, vargs);
	va_end(vargs);
}

void Platform::MessageF(MessageType type, const char *_ecv_array fmt, ...) noexcept
{
	if (!verbose) { return; }
	va_list vargs;
	va_start(vargs, fmt);
	vfprintf(stderr, fmt, vargs);
	va_end(vargs);
}

[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept
{
	char detail[300];
	snprintf(detail, sizeof(detail), "RRF_ASSERT failed at %s:%u, the board would reset", file, (unsigned int)line);
	FuzzReport("RRF_ASSERT", detail);
}

// RepRap::ReportInternalError prints "Internal Error in <func> at <file>(<line>)" and goes on; that is always a defect in RRF, so the
// target reports it
void FuzzReportInternalError(const char *file, const char *func, int line) noexcept
{
	const char *slash = strrchr(file, '/');
	char detail[300];
	snprintf(detail, sizeof(detail), "internal error in %s at %s(%d)", func, (slash == nullptr) ? file : slash + 1, line);
	char key[200];
	snprintf(key, sizeof(key), "internal-error %s", func);
	FuzzReport(key, detail);
}

[[noreturn]] void SoftwareReset(SoftwareResetReason initialReason, const uint32_t *_ecv_array _ecv_null stk) noexcept
{
	FuzzReport("software-reset-stack-overflow", "CheckStack found too little stack left to throw, the board resets");
}

void *Tasks::AllocPermanent(size_t sz, std::align_val_t align) noexcept { return ::operator new(sz, align); }

static uint64_t fuzzMillis = 0;
uint64_t millis64() noexcept { return fuzzMillis += 7; }
static uint32_t fuzzRandom = 1;
uint32_t random32() noexcept { fuzzRandom = fuzzRandom * 1103515245u + 12345u; return fuzzRandom; }
float fastSqrtf(float f) noexcept { return sqrtf(f); }

// RRFLibraries NamedEnum.h:79 declares NamedEnumLookup with an unsigned int count, NamedEnum.cpp:13 defines it with size_t; both are
// 32 bits on the target, so they are one function there. On the 64-bit host they are two overloads, and this one passes the call on
unsigned int NamedEnumLookup(c_string s, c_string const names[], size_t numNames) noexcept;
unsigned int NamedEnumLookup(c_string s, c_string const names[], unsigned int numNames) noexcept
{
	return NamedEnumLookup(s, names, (size_t)numNames);
}

const uint32_t *_ecv_array GetStackPointer() noexcept { return reinterpret_cast<const uint32_t *>(FuzzStack::StackPointer()); }
const uint32_t *TaskBase::GetCurrentTaskStackBase() noexcept { return reinterpret_cast<const uint32_t *>(FuzzStack::StackLimit()); }
TaskBase *TaskBase::GetCallerTaskHandle() noexcept { return static_cast<TaskBase *>(FuzzStack::TaskControlBlock()); }
static_assert(sizeof(TaskBase) == 128, "board_stack.cpp TcbSize");

// Every C++ exception passes here (-Wl,--wrap=__cxa_throw): the board needs StackUsage::Throw bytes of stack to throw
extern "C" [[noreturn]] void __real___cxa_throw(void *, std::type_info *, void (*)(void *));
extern "C" [[noreturn]] void __wrap___cxa_throw(void *obj, std::type_info *tinfo, void (*dest)(void *))
{
	FuzzStack::CheckThrow();
	__real___cxa_throw(obj, tinfo, dest);
}

// ---------------------------------------------------------------------------------------------------------------------
// RTOS locks for one task, see shim/ExprShim.h

void ReadWriteLock::LockForReading() noexcept
{
	if (writers != 0) { return; }								// RTOSIface.cpp:243-252: the owner of the write lock ignores read locks
	++readers;
}

void ReadWriteLock::ReleaseReader() noexcept
{
	if (writers != 0) { return; }								// RTOSIface.cpp:338-339
	RRF_ASSERT(readers != 0);
	--readers;
}

void ReadWriteLock::LockForWriting() noexcept
{
	if (writers != 0) { ++writers; return; }					// RTOSIface.cpp:407-415
	if (readers != 0)
	{
		// RTOSIface.cpp:418-447: the request is queued and the task waits until the readers have gone, which are this task
		FuzzReport("deadlock", "write lock requested while this task holds a read lock of the same lock (RTOSIface.cpp:418-447)");
	}
	writers = 1;
}

bool ReadWriteLock::ConditionalLockForWriting() noexcept
{
	if (writers != 0 || readers != 0) { return false; }
	writers = 1;
	return true;
}

void ReadWriteLock::ReleaseWriter() noexcept
{
	RRF_ASSERT(writers != 0);
	--writers;
}

void ReadWriteLock::DowngradeWriter() noexcept
{
	RRF_ASSERT(writers == 1);
	writers = 0;
	readers = 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// GCodeException.cpp: the constructors that take a G-code buffer add the file name and line of a file or file macro
// (GCodeException.cpp:15-27); the fuzz buffer runs neither (IsDoingFile and IsDoingFileMacro are false), so they are the plain ones

GCodeException::GCodeException(const GCodeBuffer *_ecv_null, int col, GCodeExceptionMessage msg) noexcept : line(-1), column(col), message(msg.GetText()) { }
GCodeException::GCodeException(const GCodeBuffer *_ecv_null p_gb, int col, GCodeExceptionMessage msg, uint32_t uparam) noexcept : GCodeException(p_gb, col, msg)
{
	param.u = uparam;
}
GCodeException::GCodeException(const GCodeBuffer *_ecv_null p_gb, int col, GCodeExceptionMessage msg, const char *_ecv_array sparam) noexcept : GCodeException(p_gb, col, msg)
{
	stringParam.copy(sparam);
}

#include "GCodeException.inc"
#include "RrfFunctions.inc"

// ---------------------------------------------------------------------------------------------------------------------
// Files: fileexists() and fileread() see these files (MassStorage::CombineName and OpenFile are played; the LineReader of
// ExpressionParser.cpp reads them through FileStore::Read)

namespace
{
	struct FuzzFile { const char *name; const char *text; };
	const FuzzFile files[] =
	{
		{ "0:/sys/config.g", "M550 P\"fuzz\"\n" },
		{ "0:/sys/data.csv", "1, 2.5, \"abc\", 'x', -3, 0x1F, \"say \"\"hi\"\"\", 4294967296, -2147483648, 1e40\nsecond line\n" },
		{ "0:/sys/dots.txt", "1.5.2.5.3\n" },
		{ "0:/sys/long.txt", "\"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789ABCDEFGHIJ\", 7\n" },
		{ "0:/sys/empty.txt", "" },
		{ "0:/sys/bad.txt", "1, x, 3\n" },
		{ "0:/sys/many.txt", "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40\n" },
	};
	FileStore theFile;
}

bool FileStore::Read(char& b) noexcept
{
	if (!open) { FuzzReport("file-read-after-close", "FileStore::Read on a closed file"); }
	if (pos >= length) { return false; }
	b = data[pos++];
	return true;
}

bool FileStore::Close() noexcept
{
	if (!open) { FuzzReport("file-double-close", "FileStore::Close on a closed file"); }
	open = false;
	return true;
}

// MassStorage.cpp: CombineName puts the directory before a file name that has no volume and returns false if the result is too long
bool MassStorage::CombineName(const StringRef& outbuf, const char *_ecv_array _ecv_null directory, const char *_ecv_array fileName) noexcept
{
	outbuf.Clear();
	bool err = false;
	if (directory != nullptr && !(isDigit(fileName[0]) && fileName[1] == ':') && fileName[0] != '/')
	{
		err = outbuf.copy(directory);
	}
	if (!err) { err = outbuf.cat(fileName); }
	return !err;
}

FileStore *_ecv_null MassStorage::OpenFile(const char *_ecv_array filePath, OpenMode mode, uint32_t preAllocSize) noexcept
{
	if (theFile.open) { FuzzReport("file-leak", "a file of an earlier fileread() is still open"); }
	for (const FuzzFile& f : files)
	{
		if (strcmp(f.name, filePath) == 0 || (strncmp(filePath, "0:/", 3) == 0 && strcmp(f.name + 7, filePath + 3) == 0))
		{
			theFile.data = f.text;
			theFile.length = strlen(f.text);
			theFile.pos = 0;
			theFile.open = true;
			return &theFile;
		}
	}
	return nullptr;
}

bool Platform::SysFileExists(const char *_ecv_array filename) const noexcept
{
	String<MaxFilenameLength> path;
	(void)MassStorage::CombineName(path.GetRef(), "0:/sys/", filename);
	for (const FuzzFile& f : files)
	{
		if (strcmp(f.name, path.c_str()) == 0) { return true; }
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// The object model: the root (class RepRap of shim/Platform/RepRap.h) with tables written the way the real classes write them
// (Move.cpp:80-151, 168-379: arrays whose elements are the same object with another table number, the element index from
// context.GetLastIndex(), nested arrays with context.GetIndex(1)). Value types: bool, char, float, int32, uint32 (file position),
// uint64, string, date/time, IP address, MAC address, driver ID, 16/32/64-bit bitmaps, enumeration, special (sysDir), expansion board
// details, object, object array (also nested, empty and with null elements), null

namespace
{
	struct Axis { char letter; float userPosition; float max; bool homed; size_t numDrivers; };
	const Axis axes[] = { { 'X', 10.125f, 350.0f, true, 1 }, { 'Y', -2.5f, 350.0f, true, 2 }, { 'Z', 0.0f, 300.0f, false, 3 } };
	const char *const heaterNames[] = { "bed", nullptr, "nozzle" };
	const float heaterTemps[] = { 60.25f, 0.0f, 215.0f };
	const char *const toolNames[] = { "T0", nullptr, "T2 with a long name of 40 characters ...." };
	const float workplaceOffsets[9] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
	const char *const boardDetails = "MB6HC|Duet 3 Motherboard 6HC|Duet3Firmware_MB6HC.bin|Duet3Firmware_MB6HC.uf2|2026-11-23";
	MacAddress mac = { { 0xBE, 0xEF, 0xDE, 0xAD, 0xFE, 0xED } };
}

#define OBJECT_MODEL_FUNC(...)					OBJECT_MODEL_FUNC_BODY(RepRap, __VA_ARGS__)
#define OBJECT_MODEL_FUNC_IF(_condition, ...)	OBJECT_MODEL_FUNC_IF_BODY(RepRap, _condition, __VA_ARGS__)
#define OBJECT_MODEL_ARRAY_COUNT(_value)		OBJECT_MODEL_ARRAY_COUNT_BODY(RepRap, _value)
#define OBJECT_MODEL_ARRAY_VALUE(...)			OBJECT_MODEL_ARRAY_VALUE_BODY(RepRap, __VA_ARGS__)

constexpr ObjectModelArrayTableEntry RepRap::objectModelArrayTable[] =
{
	// 0. boards
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(2), OBJECT_MODEL_ARRAY_VALUE(self, 1) },
	// 1. heat.bedHeaters
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(4), OBJECT_MODEL_ARRAY_VALUE_NOSELF((int32_t)context.GetLastIndex() - 1) },
	// 2. heat.heaters: element 1 is null
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(3),
		[] (const ObjectModel *_ecv_from rawSelf, ObjectExplorationContext& context) noexcept -> ExpressionValue
		{ return (context.GetLastIndex() == 1) ? ExpressionValue(nullptr) : ExpressionValue(rawSelf, 9); } },
	// 3. tools: element 1 is null
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(3),
		[] (const ObjectModel *_ecv_from rawSelf, ObjectExplorationContext& context) noexcept -> ExpressionValue
		{ return (context.GetLastIndex() == 1) ? ExpressionValue(nullptr) : ExpressionValue(rawSelf, 8); } },
	// 4. move.axes
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(ARRAY_SIZE(axes)), OBJECT_MODEL_ARRAY_VALUE(self, 5) },
	// 5. move.extruders: empty
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(0), OBJECT_MODEL_ARRAY_VALUE(self, 5) },
	// 6. move.axes[].drivers: nested, as Move.cpp:100-106
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(axes[context.GetLastIndex()].numDrivers),
		OBJECT_MODEL_ARRAY_VALUE_NOSELF(DriverId((uint8_t)(context.GetIndex(1) + 1), (uint8_t)context.GetLastIndex())) },
	// 7. move.axes[].workplaceOffsets: nested, as Move.cpp:108-113
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(ARRAY_SIZE(workplaceOffsets)),
		OBJECT_MODEL_ARRAY_VALUE_NOSELF(workplaceOffsets[context.GetLastIndex()] * (float)context.GetIndex(1), 3) },
	// 8. network.interfaces
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(1), OBJECT_MODEL_ARRAY_VALUE(self, 11) },
	// 9. state.gpOut: empty
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(0), OBJECT_MODEL_ARRAY_VALUE(nullptr) },
	// 10. tools[].offsets
	{ nullptr, OBJECT_MODEL_ARRAY_COUNT_NOSELF(3), OBJECT_MODEL_ARRAY_VALUE_NOSELF((float)context.GetLastIndex() * 0.5f, 2) },
	// 11. global-like array of strings: state.names, locked like the real lists that have a lock
	{ &reprap.globalsLock, OBJECT_MODEL_ARRAY_COUNT_NOSELF(3), OBJECT_MODEL_ARRAY_VALUE_NOSELF(toolNames[context.GetLastIndex()]) },
};

DEFINE_GET_OBJECT_MODEL_ARRAY_TABLE(RepRap)

constexpr ObjectModelTableEntry RepRap::objectModelTable[] =
{
	// 0. root
	{ "boards",			OBJECT_MODEL_FUNC_ARRAY(0),											ObjectModelEntryFlags::live },
	{ "heat",			OBJECT_MODEL_FUNC(self, 2),											ObjectModelEntryFlags::live },
	{ "job",			OBJECT_MODEL_FUNC(self, 3),											ObjectModelEntryFlags::live },
	{ "move",			OBJECT_MODEL_FUNC(self, 4),											ObjectModelEntryFlags::live },
	{ "network",		OBJECT_MODEL_FUNC(self, 6),											ObjectModelEntryFlags::none },
	{ "state",			OBJECT_MODEL_FUNC(self, 7),											ObjectModelEntryFlags::live },
	{ "tools",			OBJECT_MODEL_FUNC_ARRAY(3),											ObjectModelEntryFlags::live },

	// 1. boards[]
	{ "canAddress",		OBJECT_MODEL_FUNC_NOSELF((int32_t)context.GetLastIndex()),			ObjectModelEntryFlags::none },
	{ "firmwareDate",	OBJECT_MODEL_FUNC_NOSELF(boardDetails, ExpansionDetail::firmwareDate),	ObjectModelEntryFlags::none },
	{ "firmwareFileName", OBJECT_MODEL_FUNC_NOSELF(boardDetails, ExpansionDetail::firmwareFileNameBin),	ObjectModelEntryFlags::none },
	{ "name",			OBJECT_MODEL_FUNC_NOSELF(boardDetails, ExpansionDetail::longName),	ObjectModelEntryFlags::none },
	{ "shortName",		OBJECT_MODEL_FUNC_NOSELF(boardDetails, ExpansionDetail::shortName),	ObjectModelEntryFlags::none },

	// 2. heat
	{ "bedHeaters",		OBJECT_MODEL_FUNC_ARRAY(1),											ObjectModelEntryFlags::none },
	{ "coldExtrudeTemperature", OBJECT_MODEL_FUNC_NOSELF(160.0f, 1),						ObjectModelEntryFlags::none },
	{ "heaters",		OBJECT_MODEL_FUNC_ARRAY(2),											ObjectModelEntryFlags::live },

	// 3. job
	{ "file",			OBJECT_MODEL_FUNC(self, 10),										ObjectModelEntryFlags::none },
	{ "filePosition",	OBJECT_MODEL_FUNC_NOSELF((FilePosition)4000000000u),				ObjectModelEntryFlags::live },
	{ "lastFileName",	OBJECT_MODEL_FUNC_NOSELF(nullptr),									ObjectModelEntryFlags::none },
	{ "layer",			OBJECT_MODEL_FUNC_NOSELF((int32_t)2147483647),						ObjectModelEntryFlags::live },

	// 4. move
	{ "axes",			OBJECT_MODEL_FUNC_ARRAY(4),											ObjectModelEntryFlags::live },
	{ "extruders",		OBJECT_MODEL_FUNC_ARRAY(5),											ObjectModelEntryFlags::live },
	{ "speedFactor",	OBJECT_MODEL_FUNC_NOSELF(1.0f, 2),									ObjectModelEntryFlags::none },

	// 5. move.axes[]
	{ "drivers",		OBJECT_MODEL_FUNC_ARRAY(6),											ObjectModelEntryFlags::none },
	{ "homed",			OBJECT_MODEL_FUNC_NOSELF(axes[context.GetLastIndex()].homed),		ObjectModelEntryFlags::live },
	{ "letter",			OBJECT_MODEL_FUNC_NOSELF(axes[context.GetLastIndex()].letter),		ObjectModelEntryFlags::none },
	{ "max",			OBJECT_MODEL_FUNC_NOSELF(axes[context.GetLastIndex()].max, 1),		ObjectModelEntryFlags::none },
	{ "userPosition",	OBJECT_MODEL_FUNC_NOSELF(axes[context.GetLastIndex()].userPosition, 3),	ObjectModelEntryFlags::live },
	{ "workplaceOffsets", OBJECT_MODEL_FUNC_ARRAY(7),										ObjectModelEntryFlags::none },

	// 6. network
	{ "interfaces",		OBJECT_MODEL_FUNC_ARRAY(8),											ObjectModelEntryFlags::none },
	{ "name",			OBJECT_MODEL_FUNC_NOSELF("fuzz printer"),							ObjectModelEntryFlags::none },

	// 7. state
	{ "bits16",			OBJECT_MODEL_FUNC_NOSELF(Bitmap<uint16_t>::MakeFromRaw(0xA5A5u)),				ObjectModelEntryFlags::none },
	{ "bits32",			OBJECT_MODEL_FUNC_NOSELF(Bitmap<uint32_t>::MakeFromRaw(0x80000001u)),			ObjectModelEntryFlags::none },
	{ "bits64",			OBJECT_MODEL_FUNC_NOSELF(Bitmap<uint64_t>::MakeFromRaw(0x00F0000000000001ull)),	ObjectModelEntryFlags::none },
	{ "gpOut",			OBJECT_MODEL_FUNC_ARRAY(9),											ObjectModelEntryFlags::none },
	{ "msUpTime",		OBJECT_MODEL_FUNC_NOSELF((int32_t)(context.GetStartMillis() % 1000u)),	ObjectModelEntryFlags::live },
	{ "names",			OBJECT_MODEL_FUNC_ARRAY(11),										ObjectModelEntryFlags::none },
	{ "nextTool",		OBJECT_MODEL_FUNC_NOSELF((int32_t)-1),								ObjectModelEntryFlags::live },
	{ "status",			OBJECT_MODEL_FUNC_NOSELF("idle"),									ObjectModelEntryFlags::live },
	{ "sysDir",			OBJECT_MODEL_FUNC_NOSELF(ExpressionValue::SpecialType::sysDir, 0),	ObjectModelEntryFlags::none },
	{ "time",			OBJECT_MODEL_FUNC_NOSELF(DateTime(1763900000)),						ObjectModelEntryFlags::none },
	{ "upTime",			OBJECT_MODEL_FUNC_NOSELF((int32_t)12345),							ObjectModelEntryFlags::live },
	{ "uptime64",		OBJECT_MODEL_FUNC_NOSELF((uint64_t)0x00FFFFFFFFFFFFFFull),			ObjectModelEntryFlags::none },

	// 8. tools[]
	{ "name",			OBJECT_MODEL_FUNC_NOSELF(toolNames[context.GetLastIndex()]),		ObjectModelEntryFlags::none },
	{ "number",			OBJECT_MODEL_FUNC_NOSELF((int32_t)context.GetLastIndex()),			ObjectModelEntryFlags::none },
	{ "offsets",		OBJECT_MODEL_FUNC_ARRAY(10),										ObjectModelEntryFlags::none },

	// 9. heat.heaters[]
	{ "current",		OBJECT_MODEL_FUNC_NOSELF(heaterTemps[context.GetLastIndex()], 1),	ObjectModelEntryFlags::live },
	{ "name",			OBJECT_MODEL_FUNC_NOSELF(heaterNames[context.GetLastIndex()]),		ObjectModelEntryFlags::none },
	{ "state",			OBJECT_MODEL_FUNC_NOSELF("active"),									ObjectModelEntryFlags::live },

	// 10. job.file
	{ "fileName",		OBJECT_MODEL_FUNC_NOSELF("0:/gcodes/fuzz.gcode"),					ObjectModelEntryFlags::none },
	{ "lastModified",	OBJECT_MODEL_FUNC_NOSELF(DateTime(1763800000)),						ObjectModelEntryFlags::none },
	{ "size",			OBJECT_MODEL_FUNC_NOSELF((uint64_t)5000000000ull),					ObjectModelEntryFlags::none },

	// 11. network.interfaces[]
	{ "actualIP",		OBJECT_MODEL_FUNC_NOSELF(IPAddress(0x0100A8C0u)),					ObjectModelEntryFlags::none },
	{ "mac",			OBJECT_MODEL_FUNC_NOSELF(mac),										ObjectModelEntryFlags::none },
	{ "speed",			OBJECT_MODEL_FUNC_NOSELF(nullptr),									ObjectModelEntryFlags::none },
};

constexpr uint8_t RepRap::objectModelTableDescriptor[] = { 12, 7, 5, 3, 4, 3, 6, 2, 12, 3, 3, 3, 3 };

#ifdef __clang__
// clang cannot evaluate the constexpr strcmp of the ordering check of DEFINE_GET_OBJECT_MODEL_TABLE (ObjectModel.h); the g++ builds check it
# undef OMT_ORDERING_OK
# define OMT_ORDERING_OK(_class)	(true)
#endif

DEFINE_GET_OBJECT_MODEL_TABLE(RepRap)

// ---------------------------------------------------------------------------------------------------------------------
// Variables: globals once (persistent, like on the board), locals and parameters of the buffer before each input

namespace
{
	GCodeBuffer fuzzGb;
	bool initialised = false;

	ExpressionValue MakeArray(std::initializer_list<ExpressionValue> elems)
	{
		ArrayHandle ah;
		WriteLocker locker(Heap::heapLock);
		ah.Allocate(elems.size());
		size_t i = 0;
		for (const ExpressionValue& e : elems) { ExpressionValue copy(e); ah.AssignElement(i++, copy); }
		return ExpressionValue(ah);
	}

	void Insert(VariableSet& vars, const char *name, ExpressionValue v, int16_t scope) { (void)vars.InsertNew(name, v, scope); }

	void Initialise()
	{
		if (initialised) { return; }
		initialised = true;
		Insert(reprap.globals, "count", ExpressionValue((int32_t)3), 0);
		Insert(reprap.globals, "name", ExpressionValue(StringHandle("global name")), 0);
		Insert(reprap.globals, "temps", MakeArray({ ExpressionValue(1.5f, 1), ExpressionValue(2.25f, 2), ExpressionValue((int32_t)-4) }), 0);
		Insert(reprap.globals, "matrix", MakeArray({ MakeArray({ ExpressionValue((int32_t)1), ExpressionValue((int32_t)2) }),
													 MakeArray({ ExpressionValue((int32_t)3), ExpressionValue(StringHandle("four")) }),
													 MakeArray({}) }), 0);
		Insert(reprap.globals, "flag", ExpressionValue(true), 0);
		Insert(reprap.globals, "nothing", ExpressionValue(nullptr), 0);
		Insert(reprap.printMonitor.customInfo, "slicer", ExpressionValue(StringHandle("PrusaSlicer 2.9")), 0);
		Insert(reprap.printMonitor.customInfo, "layers", ExpressionValue((int32_t)120), 0);
	}

	void SetUpBuffer()
	{
		VariableSet& v = fuzzGb.variables;
		v.Clear();
		Insert(v, "i", ExpressionValue((int32_t)7), 0);
		Insert(v, "f", ExpressionValue(0.1f, 1), 0);
		Insert(v, "s", ExpressionValue(StringHandle("hello")), 0);
		Insert(v, "c", ExpressionValue('Q'), 0);
		Insert(v, "list", MakeArray({ ExpressionValue((int32_t)10), ExpressionValue((int32_t)20), ExpressionValue((int32_t)30) }), 0);
		Insert(v, "strs", MakeArray({ ExpressionValue(StringHandle("a")), ExpressionValue(StringHandle("bc")) }), 0);
		Insert(v, "big", ExpressionValue((int32_t)2147483647), 0);
		Insert(v, "S", ExpressionValue((int32_t)100), -1);					// parameters have scope -1 (Variable.h:38)
		Insert(v, "P", ExpressionValue(StringHandle("0:/macros/x.g")), -1);
		Insert(v, "A", MakeArray({ ExpressionValue(1.0f, 1), ExpressionValue(2.0f, 1) }), -1);
		fuzzGb.machineState.iterations = 2;
		fuzzGb.machineState.lineNumber = 42;
		fuzzGb.lastResult = GCodeResult::warning;
		fuzzGb.m291Result = ExpressionValue((int32_t)1);
	}

	enum class Entry : uint8_t
	{
		parse, parseNoEvaluate, parseFloat, parseInteger, parseUnsigned, parseBoolean, parseDriverId,
		parseFloatArray, parseIntArray, parseUnsignedArray, parseDriverIdArray, numEntries
	};

	void Evaluate(Entry entry, const char *text, size_t len, size_t arrayLength, bool withBuffer)
	{
		const GCodeBuffer *const gb = (withBuffer) ? &fuzzGb : nullptr;
		ExpressionParser parser(gb, text, text + len, 5);
		String<StringLength256> reply;
		switch (entry)
		{
		case Entry::parse:
			{
				const ExpressionValue val = parser.Parse(true);
				parser.CheckForExtraCharacters();
				val.AppendAsString(reply.GetRef(), true);							// as echo and the SBC evaluation report it
			}
			break;
		case Entry::parseNoEvaluate:	{ const ExpressionValue val = parser.Parse(false); val.AppendAsString(reply.GetRef(), false); } break;
		case Entry::parseFloat:			(void)parser.ParseFloat(); break;
		case Entry::parseInteger:		(void)parser.ParseInteger(); break;
		case Entry::parseUnsigned:		(void)parser.ParseUnsigned(); break;
		case Entry::parseBoolean:		(void)parser.ParseBoolean(); break;
		case Entry::parseDriverId:		(void)parser.ParseDriverId(); break;
		case Entry::parseFloatArray:	{ std::vector<float> a(arrayLength); size_t n = arrayLength; parser.ParseFloatArray(a.data(), n); } break;
		case Entry::parseIntArray:		{ std::vector<int32_t> a(arrayLength); size_t n = arrayLength; parser.ParseIntArray(a.data(), n); } break;
		case Entry::parseUnsignedArray:	{ std::vector<uint32_t> a(arrayLength); size_t n = arrayLength; parser.ParseUnsignedArray(a.data(), n); } break;
		default:						{ std::vector<DriverId> a(arrayLength); size_t n = arrayLength; parser.ParseDriverIdArray(a.data(), n); } break;
		}
		if (verbose) { fprintf(stderr, "result: %s\n", reply.c_str()); }
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2) { return 0; }
	Initialise();
	SetUpBuffer();
	const Entry entry = (Entry)(data[0] % (uint8_t)Entry::numEntries);
	const size_t arrayLength = 1 + (data[1] & 0x1F);
	const bool withBuffer = (data[1] & 0x80) == 0;
	const size_t len = size - 2;
	char *const text = static_cast<char *>(malloc(len + 1));				// exactly as long as the expression, see above
	memcpy(text, data + 2, len);

	FuzzStack::Begin(stackBytes);
	const size_t depthBefore = FuzzStack::Depth();
	try
	{
		Evaluate(entry, text, len, arrayLength, withBuffer);
	}
	catch (const GCodeException& e)
	{
		FuzzStack::SetDepth(depthBefore);			// clang does not call the exit hooks while it unwinds (g++ does)
		String<StringLength256> reply;
		e.GetMessage(reply.GetRef(), withBuffer ? &fuzzGb : nullptr);
		if (verbose) { fprintf(stderr, "exception: %s\n", reply.c_str()); }
	}
	FuzzStack::Begin(0);
	if (theFile.open) { FuzzReport("file-leak", "fileread() left its file open"); }
	if (Heap::heapLock.readers != 0 || Heap::heapLock.writers != 0) { FuzzReport("heap-lock-leak", "heapLock still held after the evaluation"); }
	free(text);
	fuzzGb.variables.Clear();
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Seeds: realistic expressions with the syntax of ExpressionParser.cpp (operators "?^&|!=<>+-*/" with && || == != <= >=, ParseInternal
// 183-735; unary - + # !; brackets ( ) { } [ ]; literals: numbers incl. hex, "strings" with "" and ' escapes, 'c' characters;
// constants false iterations line null pi result true input (ExpressionParser.cpp:79); the functions of ExpressionParser.cpp:80)

namespace
{
	const char *const seedExpressions[] =
	{
		"1+2*3", "(1+2)*3", "-5 - -3", "+7", "7/2", "7.5/0", "2147483647 + 0", "0x7FFFFFFF", "4294967295", "4294967296", "1.25e3", "3.", ".5",
		"10 > 3 && 2 < 1 || true", "1 == 1.0", "1 != 2", "3 >= 3", "2 <= 1", "!false", "true ? 1 : 2", "false ? 1 : true ? 2 : 3",
		"\"abc\" ^ \"def\"", "\"a\" ^ 1 ^ 2.5 ^ true ^ null", "\"x\" == \"x\"", "\"Say \"\"hi\"\"\"", "\"'A'b\"", "'c'", "'c' == 'c'",
		"abs(-3)", "abs(-2.5)", "min(3, 1, 2)", "max(1.5, 2)", "max({1, 5, 3})", "min(var.list)", "mod(7, 3)", "mod(7.5, 2)", "mod(-7, 0)",
		"floor(2.7)", "ceil(-2.2)", "round(2.5)", "sqrt(16)", "square(3)", "pow(2, 10)", "pow(2, -1)", "pow(2.5, 2)", "exp(1)", "log(10)",
		"sin(pi/2)", "cos(0)", "tan(1)", "asin(1)", "acos(1)", "atan(1)", "atan2(1, 1)", "degrees(pi)", "radians(180)", "isnan(0/0)",
		"random(10)", "datetime(0)", "datetime(\"2026-11-24T01:02:03\")", "datetime(1763900000) + 60", "state.time - datetime(0)",
		"+state.time", "datetime(state.uptime64)", "take(\"abcdef\", 3)", "drop(\"abcdef\", 2)", "take({1,2,3,4}, 2)", "drop({1,2,3}, 5)",
		"take(var.s, 10)", "drop(var.s, 1)", "take(move.axes, 2)", "drop(move.axes, 1)",
		"find(\"hello\", \"ll\")", "find(\"hello\", 'o')", "find(var.s, var.s)", "vector(3, 0)", "vector(2, \"x\")", "vector(0, 1)",
		"#vector(4, {1})", "exists(var.i)", "exists(var.nope)", "exists(param.S)", "exists(global.count)", "exists(move.axes[0].homed)",
		"exists(move.axes[9])", "exists(#move.axes)", "exists(var)", "exists(param)", "exists(global.matrix[1][1])",
		"fileexists(\"config.g\")", "fileexists(\"nope.g\")", "fileread(\"data.csv\", 0, 10, ',')", "fileread(\"data.csv\", 2, 3, ',')",
		"fileread(\"dots.txt\", 0, 5, '.')", "fileread(\"long.txt\", 0, 2, ',')", "fileread(\"empty.txt\", 0, 1, ',')",
		"fileread(\"bad.txt\", 0, 3, ',')", "fileread(\"many.txt\", 0, 40, ',')", "fileread(\"missing.txt\", 0, 1, ',')",
		"{1, 2, 3}", "{1,}", "{}", "[]", "[1]", "[1, 2.5, \"three\", 'f', true, null]", "{{1, 2}, {3, {4, 5}}}", "{1,2,3}[1]",
		"{1,2} ^ {3}", "move.axes ^ move.axes", "move.axes ^ {1}", "#{1,2,3}", "#\"hello\"", "#move.axes", "#var.list", "#global.matrix[2]",
		"\"abc\"[1]", "var.s[4]", "var.list[2]", "var.list[3]", "global.matrix[1][1]", "global.matrix[1][1][0]", "global.temps[0] * 2",
		"var.i + var.f", "var.c", "param.S + 1", "param.P ^ \".bak\"", "param.A[1]",
		"#param.A", "global.count", "global.name ^ \"!\"", "global.flag && true", "global.nothing == null", "iterations", "line", "result", "input", "pi", "null", "true", "false",
		"job.file.customInfo.slicer", "job.file.customInfo.layers + 1", "move.axes[0].letter", "move.axes[1].userPosition + 1",
		"move.axes[2].homed", "move.axes[1].drivers[1]", "move.axes[2].drivers", "#move.axes[2].drivers", "move.axes[1].workplaceOffsets[8]",
		"(move.axes)[1]", "(move.axes[1].drivers)[1]", "(move.axes[1].workplaceOffsets)[1]", "#(move.axes[1].drivers)", "(tools[0].offsets)[2]",
		"(state.names)[0]", "(heat.heaters)[1]", "(tools)[5]", "(network.interfaces)[0].actualIP", "(boards)[0]",
		"move.axes[{1}].max", "move.axes[var.i - 6].letter", "move.extruders", "#move.extruders", "move.extruders[0]", "move.speedFactor * 100",
		"heat.heaters[0].current", "heat.heaters[1]", "heat.heaters[1].current", "heat.heaters[2].name", "heat.bedHeaters[0]",
		"heat.coldExtrudeTemperature", "tools[0].name", "tools[1]", "#tools", "tools[2].offsets[2]", "tools[0].offsets", "boards[0].canAddress",
		"boards[1].firmwareDate", "boards[0].name", "boards[0].shortName", "boards[0].firmwareFileName", "job.filePosition",
		"job.filePosition + 1", "job.lastFileName", "job.file.size", "job.file.size > 1", "job.file.lastModified",
		"job.file.fileName ^ \"\"", "network.name", "network.interfaces[0].actualIP", "network.interfaces[0].mac", "network.interfaces[0].speed",
		"state.status == \"idle\"", "state.upTime / 60", "state.msUpTime", "state.nextTool < 0", "state.sysDir", "state.sysDir ^ \"x\"",
		"state.bits16", "#state.bits32", "state.bits64", "state.gpOut", "#state.gpOut", "state.names[0]", "state.names", "state.time",
		"state.uptime64", "boards[0]", "move", "move.axes", "move.axes[0]", "heat.heaters", "{2.5, 3, 4}",
		"{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}", "{0.1, 1.2}", "{3}", "{var.list}", "{move.axes[1].workplaceOffsets}",
		"{global.temps}", "1.2", "3", "-1", "{1.0, 2.3}", "{1, 2.1}", "(((((((((((1)))))))))))", "{{{{{{{{{{1}}}}}}}}}}", "----------1",
		"!!!!!!!!!!true", "#####\"abc\"", "1 + (2 + (3 + (4 + (5 + (6 + (7 + 8))))))", "var.nope", "global.nope", "param.Q", "nofunc(1)",
		"abs()", "max(1,)", "\"unterminated", "'", "{1, 2", "move.axes[", "move..axes", "1 +", "move.axes[-1]", "var.list[-1]",
	};

}

static std::vector<std::vector<uint8_t>> FuzzSeeds();
constexpr size_t FuzzMaxInputSize = 1024;
const char *const FuzzKeyFiles[] = { "ExpressionParser.cpp", "ObjectModel.cpp", "Variable.cpp", "Heap.cpp", "StringHandle.cpp", "ArrayHandle.cpp",
	"strptime.cpp", "StringRef.cpp", "SafeVsnprintf.cpp", "NumericConverter.cpp", "StringFunctions.cpp", "SafeStrtod.cpp", "NamedEnum.cpp", "IP4String.cpp" };

#ifdef FUZZ_ASAN
static void FuzzDescribeAddress(uintptr_t) { }
#endif

#ifdef FUZZ_TAKE_DROP_CHECK

// ---------------------------------------------------------------------------------------------------------------------
// Fixed cases of take() and drop() of a heap string (make take-drop, build/asan-take-drop-check): the allocation of the result
// garbage-collects when no heap block has room for it and enough space is waiting to be recycled (Heap::AllocateSpace), and the
// collection moves the strings down with memmove (Heap::GarbageCollectInternal). Each case lays out one heap block so that the
// allocation of the result collects and moves the operand, evaluates the expression with the real parser, and checks the text of the
// result and that a collection ran.

namespace
{
	constexpr size_t HeapBlockSize = 2048;										// as in Heap.cpp

	// The space of a string of 'len' characters in the heap: StringHandle::InternalAssign asks for the StorageSpace, the characters and
	// the null, and Heap::AllocateSpace rounds it up to a multiple of 4
	size_t SpaceOf(size_t len) noexcept { return (sizeof(Heap::StorageSpace) + len + 1 + 3u) & ~(size_t)3u; }

	struct HeapState { unsigned int allocated = 0, used = 0, recyclable = 0, gcCycles = 0; };

	HeapState GetHeapState() noexcept
	{
		String<StringLength256> s;
		Platform platform;
		Heap::Diagnostics(s.GetRef(), platform);
		HeapState h;
		const char *const p = strstr(s.c_str(), "allocated/used/recyclable ");
		const char *const q = strstr(s.c_str(), "gc cycles ");
		if (p == nullptr || q == nullptr
			|| sscanf(p, "allocated/used/recyclable %u/%u/%u", &h.allocated, &h.used, &h.recyclable) != 3
			|| sscanf(q, "gc cycles %u", &h.gcCycles) != 1)
		{
			fprintf(stderr, "cannot read the heap diagnostics: %s\n", s.c_str());
			abort();
		}
		return h;
	}

	unsigned int takeDropFailures = 0;

	// Lay out the heap block as [garbage][operand][name of var.op][filler to the end of the block], release the garbage, evaluate
	// 'expression' and compare the result with 'expected'. The heap block must be empty before.
	void CheckTakeDrop(const char *expression, const std::string& operand, const std::string& expected) noexcept
	{
		std::string why;
		const HeapState before = GetHeapState();
		if (before.used != 0 || before.allocated > HeapBlockSize)
		{
			why = " the heap is not empty before the case (used " + std::to_string(before.used) + ", allocated " + std::to_string(before.allocated) + ")";
		}
		else
		{
			const size_t garbageLength = 1000;
			AutoStringHandle *garbage = new AutoStringHandle(std::string(garbageLength, 'g').c_str());
			fuzzGb.variables.Clear();
			Insert(fuzzGb.variables, "op", ExpressionValue(StringHandle(operand.c_str())), 0);
			const size_t filled = SpaceOf(garbageLength) + SpaceOf(operand.size()) + SpaceOf(strlen("op"));
			const AutoStringHandle filler(std::string(HeapBlockSize - filled - sizeof(Heap::StorageSpace) - 1, 'x').c_str());
			const HeapState full = GetHeapState();
			delete garbage;												// its space waits to be recycled
			const HeapState laidOut = GetHeapState();
			if (full.allocated != HeapBlockSize || full.used != HeapBlockSize || laidOut.recyclable != SpaceOf(garbageLength))
			{
				why = " the heap is not laid out as intended (allocated " + std::to_string(full.allocated) + ", used " + std::to_string(full.used)
						+ ", recyclable " + std::to_string(laidOut.recyclable) + ")";
			}
			else
			{
				std::string result;
				FuzzStack::Begin(stackBytes);
				try
				{
					ExpressionParser parser(&fuzzGb, expression, expression + strlen(expression), 5);
					const ExpressionValue val = parser.Parse(true);
					if (val.GetType() == TypeCode::HeapString)
					{
						result = val.shVal.Get().Ptr();
					}
					else
					{
						why = " the result is not a heap string";
					}
				}
				catch (const GCodeException& e)
				{
					String<StringLength256> reply;
					e.GetMessage(reply.GetRef(), &fuzzGb);
					why = std::string(" exception: ") + reply.c_str();
				}
				FuzzStack::Begin(0);
				const HeapState after = GetHeapState();
				if (why.empty() && result != expected) { why = " result \"" + result + "\" instead of \"" + expected + "\""; }
				if (after.gcCycles != laidOut.gcCycles + 1)
				{
					why += " " + std::to_string(after.gcCycles - laidOut.gcCycles) + " garbage collections instead of 1 while the result was allocated";
				}
			}
		}

		// Release everything and collect, so that the next case finds the heap block empty
		fuzzGb.variables.Clear();
		Heap::GarbageCollect();
		if (why.empty())
		{
			printf("PASS %s\n", expression);
		}
		else
		{
			++takeDropFailures;
			printf("FAIL %s:%s\n", expression, why.c_str());
		}
	}

	void Report(const std::string& name, const std::string& why) noexcept
	{
		if (why.empty())
		{
			printf("PASS %s\n", name.c_str());
		}
		else
		{
			++takeDropFailures;
			printf("FAIL %s:%s\n", name.c_str(), why.c_str());
		}
	}

	// Evaluate an expression with the real parser and return its value as echo prints it, or the message of its exception
	std::string EvaluateToString(const char *expression) noexcept
	{
		String<StringLength256> reply;
		FuzzStack::Begin(stackBytes);
		try
		{
			ExpressionParser parser(&fuzzGb, expression, expression + strlen(expression), 5);
			const ExpressionValue val = parser.Parse(true);
			val.AppendAsString(reply.GetRef(), true);
		}
		catch (const GCodeException& e)
		{
			reply.copy("exception: ");
			String<StringLength256> message;
			e.GetMessage(message.GetRef(), &fuzzGb);
			reply.cat(message.c_str());
		}
		FuzzStack::Begin(0);
		return reply.c_str();
	}

	// A nested object model array (move.axes[n].drivers and .workplaceOffsets, whose functions read the parent index with
	// context.GetIndex(1) and the number of elements of axis n with context.GetLastIndex(), as Move.cpp:100-113) must give the elements of
	// axis n through take(), drop() and the array parsers, the same as the index operator, which puts the parent index into the context
	void CheckSameValue(const char *expression, const char *reference) noexcept
	{
		const std::string got = EvaluateToString(expression), expected = EvaluateToString(reference);
		Report(std::string(expression) + " is " + reference, (got == expected) ? "" : " \"" + got + "\" instead of \"" + expected + "\"");
	}

	void CheckFloatArray(const char *expression, const std::vector<float>& expected) noexcept
	{
		std::string why;
		float values[32];
		size_t length = ARRAY_SIZE(values);
		FuzzStack::Begin(stackBytes);
		try
		{
			ExpressionParser parser(&fuzzGb, expression, expression + strlen(expression), 5);
			parser.ParseFloatArray(values, length);
			if (length != expected.size()) { why = " " + std::to_string(length) + " elements instead of " + std::to_string(expected.size()); }
			for (size_t i = 0; why.empty() && i < length; ++i)
			{
				if (values[i] != expected[i]) { why = " element " + std::to_string(i) + " is " + std::to_string(values[i]) + " instead of " + std::to_string(expected[i]); }
			}
		}
		catch (const GCodeException& e)
		{
			why = " exception";
		}
		FuzzStack::Begin(0);
		Report(std::string("ParseFloatArray ") + expression, why);
	}

	void CheckDriverIdArray(const char *expression, unsigned int board, unsigned int numDrivers) noexcept
	{
		std::string why;
		DriverId values[32];
		size_t length = ARRAY_SIZE(values);
		FuzzStack::Begin(stackBytes);
		try
		{
			ExpressionParser parser(&fuzzGb, expression, expression + strlen(expression), 5);
			parser.ParseDriverIdArray(values, length);
			if (length != numDrivers) { why = " " + std::to_string(length) + " elements instead of " + std::to_string(numDrivers); }
			for (size_t i = 0; why.empty() && i < length; ++i)
			{
				if (values[i].boardAddress != board || values[i].localDriver != i)
				{
					why = " element " + std::to_string(i) + " is " + std::to_string(values[i].boardAddress) + "." + std::to_string(values[i].localDriver);
				}
			}
		}
		catch (const GCodeException& e)
		{
			why = " exception";
		}
		FuzzStack::Begin(0);
		Report(std::string("ParseDriverIdArray ") + expression, why);
	}

	int RunTakeDropChecks() noexcept
	{
		const std::string operand = "0123456789abcdefghijklmnopqrstuvwxyzABCD";
		CheckTakeDrop("take(var.op, 5)", operand, operand.substr(0, 5));
		CheckTakeDrop("take(var.op, 100)", operand, operand);					// as long as the operand
		CheckTakeDrop("drop(var.op, 30)", operand, operand.substr(30));
		CheckTakeDrop("drop(var.op, 1)", operand, operand.substr(1));

		// Nested object model arrays (finding E4). The stand-in axes have 1, 2 and 3 drivers; driver i of axis n is (n + 1).i, and
		// workplace offset i of axis n is i * n
		CheckSameValue("take(move.axes[1].drivers, 100)", "move.axes[1].drivers");
		CheckSameValue("take(move.axes[2].drivers, 1)", "{(move.axes[2].drivers)[0],}");
		CheckSameValue("take(move.axes[2].workplaceOffsets, 2)", "{(move.axes[2].workplaceOffsets)[0], (move.axes[2].workplaceOffsets)[1]}");
		CheckSameValue("drop(move.axes[1].drivers, 0)", "move.axes[1].drivers");
		CheckSameValue("drop(move.axes[2].drivers, 2)", "{(move.axes[2].drivers)[2],}");
		CheckSameValue("drop(move.axes[2].workplaceOffsets, 7)", "{(move.axes[2].workplaceOffsets)[7], (move.axes[2].workplaceOffsets)[8]}");
		CheckFloatArray("{move.axes[2].workplaceOffsets}", { 0.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f, 14.0f, 16.0f });
		CheckDriverIdArray("{move.axes[2].drivers}", 3, 3);
		CheckDriverIdArray("{move.axes[0].drivers}", 1, 1);

		// Float to int32 range of floor, ceil, round and pow (finding E3): (float)INT32_MAX is 2^31, which must stay a float. Compared
		// with == so that no INT_MIN is printed (B6 of ../binary-code-fuzz, RRFLibraries)
		CheckSameValue("floor(2147483648.0) == 2147483648.0", "true");
		CheckSameValue("ceil(2147483647.5) == 2147483648.0", "true");
		CheckSameValue("round(2147483648.0) == 2147483648.0", "true");
		CheckSameValue("pow(2, 31) == 2147483648.0", "true");
		CheckSameValue("floor(2147483520.0)", "2147483520");					// the largest float below 2^31 is still an int
		CheckSameValue("pow(2, 30)", "1073741824");
		CheckSameValue("round(-2147483648.0) == -2147483647 - 1", "true");
		CheckSameValue("pow(-2, 31) == -2147483647 - 1", "true");

		printf("%u failed\n", takeDropFailures);
		return (takeDropFailures == 0) ? 0 : 1;
	}
}

int main() { return RunTakeDropChecks(); }

#elif !defined(FUZZ_LIBFUZZER)
# include "../sbc-packet-fuzz/fuzz_driver.inc"
#endif

static std::vector<std::vector<uint8_t>> FuzzSeeds()
{
	std::vector<std::vector<uint8_t>> seeds;
	for (const char *e : seedExpressions)
	{
		const size_t len = strlen(e);
		const bool startsWithBrace = (e[0] == '{');
		for (uint8_t entry = 0; entry < (uint8_t)Entry::numEntries; ++entry)
		{
			// The array parsers are called when the current character is '{' (ExpressionParser.cpp:870), the others for any expression;
			// the general parser gets every seed, the typed parsers the short ones
			if (entry >= (uint8_t)Entry::parseFloatArray && !startsWithBrace) { continue; }
			if (entry >= 2 && entry < (uint8_t)Entry::parseFloatArray && len > 24) { continue; }
			std::vector<uint8_t> b;
			b.push_back(entry);
			b.push_back((entry == 0 && len % 3 == 0) ? 0x80 : 0x07);			// some with no G-code buffer, as from SbcInterface.cpp:793 with gb null
			b.insert(b.end(), e, e + len);
			seeds.push_back(b);
		}
	}
	return seeds;
}

// FUZZ_STACK_BYTES sets the board stack that is left when the parser is called (default 4096 bytes, see README.md)
namespace
{
	struct ConfigFromEnvironment
	{
		ConfigFromEnvironment() noexcept
		{
			const char *const s = getenv("FUZZ_STACK_BYTES");
			if (s != nullptr) { stackBytes = strtoull(s, nullptr, 0); }
		}
	} configFromEnvironment;
}
