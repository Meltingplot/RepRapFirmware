/*
 * PublishedPtr.h
 *
 *  Created on: 21 Nov 2026
 *      Author: Tim Schneider
 *
 * A PublishedPtr points to an object that one task, the writer, creates, replaces and deletes, while other tasks and ISRs read it.
 * The rules are:
 * R1. An object must not be deleted while a PublishedPtr points to it. Replace and Delete stop pointing to the object before they delete it.
 *     Publishable counts the PublishedPtrs that point to an object, and its destructor catches any other deletion of a published object.
 * R2. Only fully built objects may be published. The writer builds the object first and then passes it to Publish or Replace, because a
 *     reader may use it as soon as the PublishedPtr points to it. This cannot be checked.
 * R3. A reader may read without the lock only if the writer cannot run until the reader has finished with the object, and it must not block
 *     while it uses the object. That holds for ISRs, for the writer task itself, while interrupts are masked or the scheduler is suspended,
 *     and for tasks that have a higher priority than the writer. Every other reader must hold the lock (GetLock) for reading while it uses
 *     the object, so that Publish, Replace and Delete wait until it has finished. Get checks this and aborts if the rule is broken.
 */

#ifndef SRC_PLATFORM_PUBLISHEDPTR_H_
#define SRC_PLATFORM_PUBLISHEDPTR_H_

#include <RepRapFirmware.h>
#include <RTOSIface/RTOSIface.h>

// Base class of the objects that a PublishedPtr can point to
class Publishable
{
public:
	Publishable() noexcept : publishCount(0) { }
	Publishable(const Publishable&) noexcept : publishCount(0) { }				// a copy is a new object, which nothing points to yet
	Publishable& operator=(const Publishable&) noexcept { return *this; }		// assigning to an object doesn't change what points to it

protected:
	~Publishable() noexcept { RRF_ASSERT(publishCount == 0); }					// R1: an object must not be deleted while a PublishedPtr points to it

private:
	friend class PublishedPtrBase;

	uint8_t publishCount;														// how many PublishedPtrs point to this object, changed only by their writer
};

// The part of PublishedPtr that doesn't depend on the type of the object
class PublishedPtrBase
{
public:
	// Return the lock that readers which may not read without it (R3) must hold for reading while they use the object
	ReadWriteLock& GetLock() const noexcept { return lock; }

	PublishedPtrBase(const PublishedPtrBase&) = delete;
	PublishedPtrBase& operator=(const PublishedPtrBase&) = delete;

protected:
	explicit PublishedPtrBase(TaskHandle p_writer) noexcept : writer(p_writer) { }

	void CheckReader() const noexcept;											// R3: abort if the caller may not read without the lock and doesn't hold it
	void CheckWriter() const noexcept { RRF_ASSERT(RTOSIface::GetCurrentTask() == writer); }

	static void Published(Publishable *_ecv_null p) noexcept { if (p != nullptr) { ++p->publishCount; } }
	static void Unpublished(Publishable *_ecv_null p) noexcept { if (p != nullptr) { --p->publishCount; } }

	const TaskHandle writer;													// the only task that may change what we point to
	mutable ReadWriteLock lock;													// held for writing while we change what we point to
};

template<class T> class PublishedPtr : public PublishedPtrBase
{
public:
	// The writer is the task that publishes, replaces and deletes the objects. The initial object must already be fully built (R2).
	PublishedPtr(T *_ecv_null p_object, TaskHandle p_writer) noexcept : PublishedPtrBase(p_writer), object(p_object) { Published(p_object); }

	// Return the object. The caller must be allowed to read without the lock or hold the lock (R3).
	T *_ecv_null Get() const noexcept { CheckReader(); return object; }
	T *_ecv_null operator->() const noexcept { return Get(); }
	T& operator*() const noexcept { return *_ecv_not_null(Get()); }

	// Writer only. Point to a new object and return the old one, which is no longer published. No reader that holds the lock can still be using it,
	// but readers can still reach it if the new object links to it, as a pushed machine state links to the previous one. Otherwise the caller owns it.
	T *_ecv_null Publish(T *_ecv_null pNew) noexcept;

	// Writer only. Point to a new object, then delete the old one. Like ReplaceObject in RepRapFirmware.h, but it waits for readers that hold the lock.
	void Replace(T *_ecv_null pNew) noexcept { delete Publish(pNew); }

	// Writer only. Point to no object, then delete the old one.
	void Delete() noexcept { Replace(nullptr); }

private:
	T *_ecv_null volatile object;
};

template<class T> T *_ecv_null PublishedPtr<T>::Publish(T *_ecv_null pNew) noexcept
{
	CheckWriter();
	Published(pNew);
	T *_ecv_null pOld;
	{
		WriteLocker locker(lock);												// wait until no reader that holds the lock can still be using the old object
		pOld = object;
		object = pNew;
	}
	Unpublished(pOld);
	return pOld;
}

#endif /* SRC_PLATFORM_PUBLISHEDPTR_H_ */
