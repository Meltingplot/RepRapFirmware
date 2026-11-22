// Host stand-in for RRFLibraries src/RTOSIface/RTOSIface.h: what src/Platform/PublishedPtr.h uses from it. The test runs in one task, the
// main task, so the lock never has to wait. Tools/published-ptr-test runs PublishedPtr with the real ReadWriteLock.

#ifndef KINEMATICS_TEST_RTOSIFACE_H
#define KINEMATICS_TEST_RTOSIFACE_H

class TaskBase { };
typedef TaskBase *TaskHandle;

namespace RTOSIface
{
	TaskHandle GetCurrentTask() noexcept;
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
