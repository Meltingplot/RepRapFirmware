// Host stand-ins for everything the real Platform/Heap.cpp and Platform/StringHandle.cpp need from RRF, RRFLibraries and
// FreeRTOS. The test runs in one thread and plays the other task itself, so the lock only has to count its owners for the
// CheckHasWriteLock calls in Heap.cpp. RRF_ASSERT records the failure and leaves the function: on the board vAssertCalled
// resets the processor at that point, here the test reports it and goes on with a heap that it no longer trusts.

#ifndef HEAP_TEST_SHIM_H
#define HEAP_TEST_SHIM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <atomic>
#include <new>
#include <utility>

// eCv annotations (RRFLibraries ecv_original.h)
#define _ecv_array
#define _ecv_from
#define _ecv_null
#define null
#define _ecv_not_null(x)		(x)
#define not_null(x)				(x)
#define pre(...)
#define post(...)

#define SAME70					1

// RepRapFirmware.h
#define THROWS(...)				noexcept(false)

void HeapTestAssertFailed(uint32_t line, const char *file) noexcept;
#define RRF_ASSERT(_expr) do { if (!(_expr)) { HeapTestAssertFailed(__LINE__, __FILE__); return; } } while (false)

template<class X> constexpr X min(X a, X b) noexcept { return (a < b) ? a : b; }
template<class X> constexpr X max(X a, X b) noexcept { return (a > b) ? a : b; }

constexpr size_t StringLength256 = 256;

class Platform;

// RRFLibraries General/SafeStrncpy
inline void SafeStrncpy(char *dst, const char *src, size_t length) noexcept
{
	if (length != 0)
	{
		strncpy(dst, src, length);
		dst[length - 1] = 0;
	}
}

// RRFLibraries StringRef and String<N>, as far as Heap.cpp uses them
class StringRef
{
public:
	StringRef(char *pp, size_t pl) noexcept : p(pp), len(pl) { }
	const char *c_str() const noexcept { return p; }
	int printf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(p, len, fmt, vargs);
		va_end(vargs);
		return ret;
	}
	int catf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		const size_t n = strlen(p);
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(p + n, len - n, fmt, vargs);
		va_end(vargs);
		return ret;
	}
	int lcatf(const char *fmt, ...) const noexcept __attribute__((format(printf, 2, 3)))
	{
		size_t n = strlen(p);
		if (n != 0 && n + 1 < len)
		{
			p[n++] = '\n';
			p[n] = 0;
		}
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(p + n, len - n, fmt, vargs);
		va_end(vargs);
		return ret;
	}
	int lcat(const char *s) const noexcept { return lcatf("%s", s); }
private:
	char *p;
	size_t len;
};

template<size_t N> class String
{
public:
	String() noexcept { buf[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(buf, N + 1); }
	const char *c_str() const noexcept { return buf; }
private:
	char buf[N + 1];
};

// Platform/Tasks.h
namespace Tasks
{
	inline void *AllocPermanent(size_t sz, std::align_val_t align = (std::align_val_t)__STDCPP_DEFAULT_NEW_ALIGNMENT__) noexcept
	{
		return ::operator new(sz, align);
	}
}

// RRFLibraries RTOSIface/RTOSIface.h: ReadWriteLock and its lockers, counting the owners only
class ReadWriteLock
{
public:
	ReadWriteLock() noexcept : readers(0), writers(0) { }

	void LockForReading() noexcept { ++readers; }
	void ReleaseReader() noexcept { --readers; }
	void LockForWriting() noexcept { ++writers; }
	void ReleaseWriter() noexcept { --writers; }
	void CheckHasWriteLock() noexcept;
	void CheckHasReadLock() noexcept;
	void CheckHasReadOrWriteLock() noexcept;

	unsigned int Readers() const noexcept { return readers; }
	unsigned int Writers() const noexcept { return writers; }

	ReadWriteLock(const ReadWriteLock&) = delete;
	ReadWriteLock& operator=(const ReadWriteLock&) = delete;

private:
	unsigned int readers, writers;
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
	~WriteLocker() { if (lock != nullptr) { lock->ReleaseWriter(); } }

	WriteLocker(const WriteLocker&) = delete;
	WriteLocker& operator=(const WriteLocker&) = delete;

private:
	ReadWriteLock *lock;
};

// CoreN2G CoreIO.h: AtomicCriticalSectionLocker disables interrupts on the board. The test runs one task at a time, so it does nothing
class AtomicCriticalSectionLocker
{
public:
	AtomicCriticalSectionLocker() noexcept { }
	void Cancel() noexcept { }
	AtomicCriticalSectionLocker(const AtomicCriticalSectionLocker&) = delete;
	AtomicCriticalSectionLocker& operator=(const AtomicCriticalSectionLocker&) = delete;
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

#endif
