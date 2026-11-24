// Host stand-ins for what the real GCodes/GCodeBuffer/ExpressionParser.cpp, ObjectModel/ObjectModel.cpp, ObjectModel/Variable.cpp,
// Platform/Heap.cpp, Platform/StringHandle.cpp, Platform/ArrayHandle.cpp and libc/strptime.cpp need from RRF, CoreN2G and FreeRTOS.
// The string classes, NumericConverter, SafeStrtod, NamedEnum, IPAddress, IP4String, Bitmap, function_ref (RRFLibraries), the
// constants of src/Config/Configuration.h, TypeCode, GCodeException.h, StringHandle.h, ArrayHandle.h, Heap.h, Variable.h and
// ExpressionParser.h are the real ones; DriverId and GetFloatFormatString are the real text, extracted at build time
// (gen/RrfTypes.inc). What is played here is listed in README.md ("What is real and what is played").

#ifndef EXPRESSION_FUZZ_SHIM_H
#define EXPRESSION_FUZZ_SHIM_H

#include <ecv_duet3d.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cinttypes>
#include <cmath>
#include <cctype>
#include <ctime>
#include <atomic>
#include <algorithm>
#include <new>
#include <utility>

// Board configuration of the Duet 3 MB6HC release build (Pins_Duet3_MB6HC.h, RTOS build)
#define SAME70					1
#define SAME5x					0
#define SAM4E					0
#define SAM4S					0
#define SAMC21					0
#define RP2040					0
#define STM32					0
#ifndef DUET3
# define DUET3					1
#endif
#ifndef DUET3_MB6HC
# define DUET3_MB6HC			1
#endif
#define RTOS					1
#define HAS_SBC_INTERFACE		1
#define HAS_MASS_STORAGE		1
#define HAS_EMBEDDED_FILES		0
#define SUPPORT_CAN_EXPANSION	1
#define SUPPORT_ASYNC_MOVES		1

#define null					_ecv_null
#define THROWS(...)				noexcept(false)
#define SPEED_CRITICAL
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))


// CoreN2G Core.h (the type-correct character classes; char is unsigned on the target and in this build, -funsigned-char)
static inline bool isDigit(char c) noexcept { return isdigit((int)c) != 0; }
static inline bool isAlpha(char c) noexcept { return isalpha((int)c) != 0; }
static inline bool isAlnum(char c) noexcept { return isalnum((int)c) != 0; }

#include <Config/Configuration.h>
#include <General/String.h>
#include <General/StringRef.h>
#include <General/StringFunctions.h>
#include <General/SafeStrtod.h>
#include <General/Bitmap.h>
#include <General/NamedEnum.h>
#include <General/IPAddress.h>
#include <General/SimpleMath.h>
#include <General/function_ref.h>
#include <CanId.h>
#include <ObjectModel/TypeCode.h>
#include <Platform/MessageType.h>
#include <GCodeResult.h>

extern "C" void debugPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

// RepRapFirmware.h. On the board vAssertCalled resets the processor; here it ends the run with a report that the fuzz driver counts
[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept;
#define RRF_ASSERT(_expr) do { if (!(_expr)) { vAssertCalled(__LINE__, __FILE__); } } while (false)

// RepRapFirmware.h
typedef uint32_t FileHandle;
typedef uint32_t FilePosition;
const FilePosition noFilePosition = 0xFFFFFFFFu;
typedef Bitmap<uint64_t> ParameterLettersBitmap;	// RepRapFirmware.h:433 (DUET3)
enum class Module : uint8_t { Gcodes };
typedef Bitmap<uint16_t> DebugFlags;				// Platform/RepRap.h:42
constexpr float Pi = 3.141592653589793;				// RepRapFirmware.h:634
constexpr float DegreesToRadians = 3.141592653589793/180.0;	// RepRapFirmware.h:636
constexpr float RadiansToDegrees = 180.0/3.141592653589793;	// RepRapFirmware.h:637
constexpr unsigned int MaxFloatDigitsDisplayedAfterPoint = 7;	// RepRapFirmware.h:713
const char *_ecv_array GetFloatFormatString(float val, unsigned int numDigitsAfterPoint) noexcept;	// RepRapFirmware.h:714, real text in gen/

// RRFLibraries SimpleMath.h declares fastSqrtf extern when there is no FPU; the MB6HC uses vsqrt.f32 (SimpleMath.h:122), which gives the
// IEEE square root like sqrtf, defined so in expression_fuzz.cpp

// CanInterface::GetCanAddress, for the real DriverId: a main board has CAN address 0 (CanId.h, CanId::MasterAddress)
namespace CanInterface { inline CanAddress GetCanAddress() noexcept { return CanId::MasterAddress; } }

class GCodeBuffer;
class OutputBuffer;
class Platform;

// RepRapFirmware.h:35, the real function is src/libc/strptime.cpp
const char *_ecv_array _ecv_null SafeStrptime(const char *_ecv_array buf, const char *_ecv_array format, struct tm *timeptr) noexcept;
// RepRapFirmware.h:156 (ThrowGCodeException is declared in GCodes/GCodeException.h)
#define THROW_INTERNAL_ERROR	ThrowGCodeException("internal error at file " __FILE__ "(%d)", (int32_t)__LINE__)
// RepRapFirmware.h:159: reprap.ReportInternalError prints a message on the board and goes on
void FuzzReportInternalError(const char *file, const char *func, int line) noexcept;
#define REPORT_INTERNAL_ERROR do { FuzzReportInternalError((__FILE__), (__func__), (__LINE__)); } while(0)

// The real DriverId (RepRapFirmware.h), extracted at build time
#include <RrfTypes.inc>

// Platform/Tasks.h
namespace Tasks
{
	void *AllocPermanent(size_t sz, std::align_val_t align = (std::align_val_t)__STDCPP_DEFAULT_NEW_ALIGNMENT__) noexcept;
}

// CoreN2G CoreIO.h
uint64_t millis64() noexcept;
uint32_t random32() noexcept;
static inline uint32_t random(uint32_t howbig) noexcept { return (howbig == 0) ? 0 : random32() % howbig; }		// CoreIO.h:485

// CoreN2G CoreIO.h:341 GetStackPointer and RRFLibraries RTOSIface.h:250 TaskBase::GetCurrentTaskStackBase: the stack of the board task is
// played by the stack model of board_stack.cpp, see there and README.md ("CheckStack")
const uint32_t *_ecv_array GetStackPointer() noexcept;

// RRFLibraries RTOSIface/RTOSIface.h: only TaskBase as far as the stack checks use it. ObjectExplorationContext::CheckStack takes the
// stack limit as the end of the task control block (ObjectModel.cpp:640), ExpressionParser::CheckStack takes pxTaskGetCurrentStackBase
// (ExpressionParser.cpp:2664); the stack model puts the control block just below the stack, so that both give the same limit
class TaskBase
{
public:
	static const uint32_t *GetCurrentTaskStackBase() noexcept;
	static TaskBase *GetCallerTaskHandle() noexcept;
	uint32_t tcb[32];
};

// RRFLibraries RTOSIface/RTOSIface.h:427-612, played for one task. The locks keep the counts of the real ones (RTOSIface.cpp:235-448):
// a read lock taken by the owner of the write lock is ignored, a second write lock of the owner counts up. A write lock requested while
// this task holds a read lock of the same lock waits for ever on the board (RTOSIface.cpp:418-447: the request is queued until the
// readers are gone), so the stand-in reports it as a deadlock.
class ReadWriteLock
{
public:
	ReadWriteLock() noexcept { }
	void LockForReading() noexcept;
	bool ConditionalLockForReading() noexcept { LockForReading(); return true; }
	void ReleaseReader() noexcept;
	void LockForWriting() noexcept;
	bool ConditionalLockForWriting() noexcept;
	void ReleaseWriter() noexcept;
	void DowngradeWriter() noexcept;
	bool IsWriteLocked() const noexcept { return writers != 0; }
	void CheckHasWriteLock() noexcept { RRF_ASSERT(writers != 0); }
	void CheckHasReadLock() noexcept { RRF_ASSERT(readers != 0 || writers != 0); }
	void CheckHasReadOrWriteLock() noexcept { RRF_ASSERT(readers + writers != 0); }
	ReadWriteLock(const ReadWriteLock&) = delete;
	ReadWriteLock& operator=(const ReadWriteLock&) = delete;

	unsigned int readers = 0, writers = 0;
};

class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { lock->LockForReading(); }
	explicit ReadLocker(ReadWriteLock *_ecv_null p_lock) noexcept : lock(p_lock) { if (lock != nullptr) { lock->LockForReading(); } }
	ReadLocker(ReadLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~ReadLocker() { if (lock != nullptr) { lock->ReleaseReader(); } }
	void Release() noexcept { if (lock != nullptr) { lock->ReleaseReader(); lock = nullptr; } }
	ReadLocker(const ReadLocker&) = delete;
	ReadLocker& operator=(const ReadLocker&) = delete;
private:
	ReadWriteLock *_ecv_null lock;
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { lock->LockForWriting(); }
	explicit WriteLocker(ReadWriteLock *_ecv_null p_lock) noexcept : lock(p_lock) { if (lock != nullptr) { lock->LockForWriting(); } }
	WriteLocker(WriteLocker&& other) noexcept : lock(other.lock), downgraded(other.downgraded) { other.lock = nullptr; }
	~WriteLocker() { Release(); }
	void Release() noexcept
	{
		if (lock != nullptr)
		{
			if (downgraded) { lock->ReleaseReader(); } else { lock->ReleaseWriter(); }
			lock = nullptr;
		}
	}
	void Downgrade() noexcept { if (lock != nullptr && !downgraded) { lock->DowngradeWriter(); downgraded = true; } }
	WriteLocker(const WriteLocker&) = delete;
	WriteLocker& operator=(const WriteLocker&) = delete;
private:
	ReadWriteLock *_ecv_null lock;
	bool downgraded = false;
};

template<class T> class ReadLockedPointer
{
public:
	ReadLockedPointer(ReadWriteLock& p_lock, T* p_ptr) noexcept : locker(p_lock), ptr(p_ptr) { }
	ReadLockedPointer(ReadLocker& p_locker, T* p_ptr) noexcept : locker(std::move(p_locker)), ptr(p_ptr) { }
	ReadLockedPointer(std::nullptr_t, T* p_ptr) noexcept : locker(nullptr), ptr(p_ptr) { }
	ReadLockedPointer(ReadLockedPointer<T>&& other) noexcept : locker(std::move(other.locker)), ptr(other.ptr) { other.ptr = nullptr; }
	bool IsNull() const noexcept { return ptr == nullptr; }
	bool IsNotNull() const noexcept { return ptr != nullptr; }
	T* operator->() const noexcept { return ptr; }
	T& operator*() const noexcept { return *ptr; }
	T* Ptr() const noexcept { return ptr; }
	void Release() noexcept { ptr = nullptr; locker.Release(); }
	ReadLockedPointer(const ReadLockedPointer<T>&) = delete;
	ReadLockedPointer<T>& operator=(const ReadLockedPointer<T>&) = delete;
private:
	ReadLocker locker;
	T* _ecv_null ptr;
};

template<class T> class WriteLockedPointer
{
public:
	WriteLockedPointer(ReadWriteLock& p_lock, T* p_ptr) noexcept : locker(p_lock), ptr(p_ptr) { }
	WriteLockedPointer(std::nullptr_t, T* _ecv_null p_ptr) noexcept : locker(nullptr), ptr(p_ptr) { }
	bool IsNull() const noexcept { return ptr == nullptr; }
	T* operator->() const noexcept { return ptr; }
	T* Ptr() const noexcept { return ptr; }
	WriteLockedPointer(const WriteLockedPointer<T>&) = delete;
	WriteLockedPointer<T>& operator=(const WriteLockedPointer<T>&) = delete;
private:
	WriteLocker locker;
	T* _ecv_null ptr;
};

// ExpressionParser.h:36 calls FileStore::Close inline, so the stand-ins of Storage/FileStore.h and Storage/MassStorage.h are here.
// The file functions read from a small fixed set of files, see expression_fuzz.cpp
// Storage/FileStore.h:25
enum class OpenMode : uint8_t { read, write, writeWithCrc, append };

// Storage/FileStore.h:41: Read and Close as LineReader calls them (ExpressionParser.cpp:59, ExpressionParser.h:36)
class FileStore
{
public:
	bool Read(char& b) noexcept;
	bool Close() noexcept;
	const char *_ecv_array data = nullptr;
	size_t length = 0, pos = 0;
	bool open = false;
};

// Storage/MassStorage.h:86, 91
namespace MassStorage
{
	bool CombineName(const StringRef& outbuf, const char *_ecv_array _ecv_null directory, const char *_ecv_array fileName) noexcept;
	FileStore *_ecv_null OpenFile(const char *_ecv_array filePath, OpenMode mode, uint32_t preAllocSize) noexcept;
}

#endif
