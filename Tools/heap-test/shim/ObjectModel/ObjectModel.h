// Heap test stand-in for ObjectModel/ObjectModel.h: ExpressionValue and GCodeException as far as Platform/ArrayHandle.cpp and the
// test use them. The copy constructor, the assignment, Release and the destructor count the references of heap strings and arrays
// like the real ones in ObjectModel.h and ObjectModel.cpp.

#ifndef HEAP_TEST_OBJECTMODEL_H
#define HEAP_TEST_OBJECTMODEL_H

#include <HeapTestShim.h>
#include <Platform/StringHandle.h>
#include <Platform/ArrayHandle.h>
#include <ObjectModel/TypeCode.h>

class GCodeException
{
public:
	explicit GCodeException(const char *msg) noexcept : message(msg) { }
	const char *GetMessage() const noexcept { return message; }

private:
	const char *message;
};

constexpr const char *ArrayIndexOutOfRangeText = "array index out of bounds";

struct ExpressionValue final
{
	uint32_t type : 8,
			 param : 24;
	union
	{
		StringHandle shVal;
		ArrayHandle ahVal;
		uintptr_t whole;								// uint32_t in the real one, which is as big as a pointer on the target
	};

	ExpressionValue() noexcept : type((uint32_t)TypeCode::None), param(0), whole(0) { }
	explicit ExpressionValue(StringHandle h) noexcept : type((uint32_t)TypeCode::HeapString), param(0), shVal(h) { }
	explicit ExpressionValue(ArrayHandle h) noexcept : type((uint32_t)TypeCode::HeapArray), param(0), ahVal(h) { }

	ExpressionValue(const ExpressionValue& other) noexcept
		: type(other.type), param(other.param), whole(other.whole)
	{
		IncreaseRefCount();
	}

	ExpressionValue& operator=(const ExpressionValue& other) noexcept
	{
		if (&other != this)
		{
			Release();
			type = other.type;
			param = other.param;
			whole = other.whole;
			IncreaseRefCount();
		}
		return *this;
	}

	~ExpressionValue() { Release(); }

	TypeCode GetType() const noexcept { return (TypeCode)type; }

	void Release() noexcept
	{
		switch (GetType())
		{
		case TypeCode::HeapString:
			shVal.Delete();
			type = (uint32_t)TypeCode::None;
			break;

		case TypeCode::HeapArray:
			ahVal.Delete();
			type = (uint32_t)TypeCode::None;
			break;

		default:
			break;
		}
	}

private:
	void IncreaseRefCount() noexcept
	{
		switch (GetType())
		{
		case TypeCode::HeapString:
			shVal.IncreaseRefCount();
			break;

		case TypeCode::HeapArray:
			ahVal.IncreaseRefCount();
			break;

		default:
			break;
		}
	}
};

#endif
