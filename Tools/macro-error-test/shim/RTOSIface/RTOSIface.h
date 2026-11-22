// Host stand-in for RRFLibraries src/RTOSIface/RTOSIface.h: what src/Platform/PublishedPtr.h uses from it. The test plays both tasks in one
// thread, so the lock never has to wait. Tools/published-ptr-test runs PublishedPtr with the real ReadWriteLock.

#ifndef MACRO_ERROR_TEST_RTOSIFACE_H
#define MACRO_ERROR_TEST_RTOSIFACE_H

class TaskBase { };
typedef TaskBase *TaskHandle;

namespace RTOSIface
{
	TaskHandle GetCurrentTask() noexcept;							// the task that currentTask names, see macro_error_test.cpp
}

class ReadWriteLock
{
public:
	void LockForWriting() noexcept { writeLocked = true; }
	void ReleaseWriter() noexcept { writeLocked = false; }
	bool IsWriteLocked() const noexcept { return writeLocked; }

private:
	bool writeLocked = false;
};

class WriteLocker
{
public:
	explicit WriteLocker(ReadWriteLock& p_lock) noexcept : lock(p_lock) { lock.LockForWriting(); }
	~WriteLocker() noexcept { lock.ReleaseWriter(); }

private:
	ReadWriteLock& lock;
};

#endif
