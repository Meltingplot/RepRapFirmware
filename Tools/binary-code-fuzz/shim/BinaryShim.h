// Host stand-ins for what the real GCodes/GCodeBuffer/BinaryParser.cpp, Platform/Heap.cpp, Platform/StringHandle.cpp and the extracted
// text of GCodes/GCodeException.cpp need from RRF, CoreN2G and FreeRTOS. The string classes, Bitmap, NamedEnum, IPAddress, StrToU32 and
// StrHexToU32, SafeStrncpy (RRFLibraries), the length constants of src/Config/Configuration.h, TypeCode, GCodeChannel, the SBC message
// formats, GCodeException.h and StringHandle.h/Heap.h are the real ones; DriverId and GCodeBufferState are the real text, extracted at
// build time (gen/RrfTypes.inc). What is played here and in binary_code_fuzz.cpp:
// - GCodeBuffer (shim/GCodeBuffer.h): the members that BinaryParser uses, with the names of the real class;
// - ExpressionParser (shim/ExpressionParser.h): reads the first character of the expression the way the real parser does, see there;
// - ExpressionValue (shim/ObjectModel/ObjectModel.h) and VariableSet (shim/ObjectModel/Variable.h);
// - Tasks::AllocPermanent: every allocation is a separate heap block of exactly the requested size, so that AddressSanitizer sees a read
//   or write beyond the 384-byte code buffer of a GCodeBuffer or beyond a 2 KB block of the string heap;
// - the RTOS locks: one thread, the lock only counts its owners for the CheckHasWriteLock calls in Heap.cpp.

#ifndef BINARY_CODE_FUZZ_SHIM_H
#define BINARY_CODE_FUZZ_SHIM_H

#include <ecv_duet3d.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cinttypes>
#include <cmath>
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
#define HAS_SBC_INTERFACE		1
#define HAS_MASS_STORAGE		1
#define SUPPORT_CAN_EXPANSION	1
#define SUPPORT_ASYNC_MOVES		1			// Pins_Duet3_MB6HC.h

#define null					_ecv_null
#define THROWS(...)				noexcept(false)
#define SPEED_CRITICAL
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))

template<class X> constexpr X min(X a, X b) noexcept { return (a < b) ? a : b; }
template<class X> constexpr X max(X a, X b) noexcept { return (a > b) ? a : b; }

#include <Config/Configuration.h>
#include <General/String.h>
#include <General/StringRef.h>
#include <General/StringFunctions.h>
#include <General/SafeStrtod.h>
#include <General/Bitmap.h>
#include <General/NamedEnum.h>
#include <General/IPAddress.h>
#include <CanId.h>
#include <ObjectModel/TypeCode.h>
#include <Platform/MessageType.h>
#include <GCodeResult.h>

extern "C" void debugPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

// RepRapFirmware.h. On the board vAssertCalled resets the processor; here it ends the run with a report that the fuzz driver counts
[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept;
#define RRF_ASSERT(_expr) do { if (!(_expr)) { vAssertCalled(__LINE__, __FILE__); } } while (false)

// CoreN2G CoreIO.h
inline void memcpyu32(uint32_t *dst, const uint32_t *src, size_t numWords) noexcept
{
	while (numWords != 0)
	{
		*dst++ = *src++;
		--numWords;
	}
}

// RepRapFirmware.h
typedef uint32_t FileHandle;
typedef uint32_t FilePosition;
const FilePosition noFilePosition = 0xFFFFFFFFu;
typedef Bitmap<uint64_t> ParameterLettersBitmap;	// RepRapFirmware.h:433 (DUET3)
constexpr char HighestAxisLetter = 'z';				// RepRapFirmware.h:434 (DUET3)
enum class Module : uint8_t { Gcodes };
typedef Bitmap<uint16_t> DebugFlags;				// Platform/RepRap.h:42

// CanInterface::GetCanAddress, for the real DriverId: a main board has CAN address 0 (CanId.h, CanId::MasterAddress)
namespace CanInterface { inline CanAddress GetCanAddress() noexcept { return CanId::MasterAddress; } }

class GCodeBuffer;
// RepRapFirmware.h:156 (ThrowGCodeException is declared in GCodes/GCodeException.h, which BinaryParser.h includes)
#define THROW_INTERNAL_ERROR	ThrowGCodeException("internal error at file " __FILE__ "(%d)", (int32_t)__LINE__)

// The real DriverId (RepRapFirmware.h) and GCodeBufferState (GCodes/GCodeBuffer/GCodeBuffer.h), extracted at build time
#include <RrfTypes.inc>

// Platform/Tasks.h
namespace Tasks
{
	void *AllocPermanent(size_t sz, std::align_val_t align = (std::align_val_t)__STDCPP_DEFAULT_NEW_ALIGNMENT__) noexcept;
}

// RRFLibraries RTOSIface/RTOSIface.h: ReadWriteLock and its lockers, counting the owners only
class ReadWriteLock
{
public:
	void LockForReading() noexcept { ++readers; }
	void ReleaseReader() noexcept { --readers; }
	void LockForWriting() noexcept { ++writers; }
	void ReleaseWriter() noexcept { --writers; }
	void CheckHasWriteLock() noexcept { RRF_ASSERT(writers != 0); }
	void CheckHasReadLock() noexcept { RRF_ASSERT(readers != 0); }
	void CheckHasReadOrWriteLock() noexcept { RRF_ASSERT(readers + writers != 0); }

private:
	unsigned int readers = 0, writers = 0;
};

class ReadLocker
{
public:
	explicit ReadLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { lock->LockForReading(); }
	explicit ReadLocker(ReadWriteLock *p_lock) noexcept : lock(p_lock) { if (lock != nullptr) { lock->LockForReading(); } }
	ReadLocker(ReadLocker&& other) noexcept : lock(other.lock) { other.lock = nullptr; }
	~ReadLocker() { if (lock != nullptr) { lock->ReleaseReader(); } }
	void Release() noexcept { if (lock != nullptr) { lock->ReleaseReader(); lock = nullptr; } }
	ReadLocker(const ReadLocker&) = delete;
	ReadLocker& operator=(const ReadLocker&) = delete;
private:
	ReadWriteLock *lock;
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock& p_lock) noexcept : lock(&p_lock) { lock->LockForWriting(); }
	~WriteLocker() { lock->ReleaseWriter(); }
	WriteLocker(const WriteLocker&) = delete;
	WriteLocker& operator=(const WriteLocker&) = delete;
private:
	ReadWriteLock *lock;
};

template<class T> class ReadLockedPointer
{
public:
	ReadLockedPointer(ReadLocker& p_locker, T* p_ptr) noexcept : locker(std::move(p_locker)), ptr(p_ptr) { }
	ReadLockedPointer(std::nullptr_t, T* p_ptr) noexcept : locker(nullptr), ptr(p_ptr) { }
	ReadLockedPointer(ReadLockedPointer<T>&& other) noexcept : locker(std::move(other.locker)), ptr(other.ptr) { other.ptr = nullptr; }
	bool IsNull() const noexcept { return ptr == nullptr; }
	T* Ptr() const noexcept { return ptr; }
	ReadLockedPointer(const ReadLockedPointer<T>&) = delete;
	ReadLockedPointer<T>& operator=(const ReadLockedPointer<T>&) = delete;
private:
	ReadLocker locker;
	T* ptr;
};

// Platform and RepRap: the debug flags only. BinaryParser::DecodeCommand prints the code when Module::Gcodes has the bit of the channel
class Platform { };

class RepRap
{
public:
	DebugFlags GetDebugFlags(Module) const noexcept { return debugFlags; }
	DebugFlags debugFlags;
};
extern RepRap reprap;

#endif
