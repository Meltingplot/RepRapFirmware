// Binary code fuzz stand-in for ObjectModel/ObjectModel.h: ExpressionValue as far as BinaryParser and the stand-in ExpressionParser use
// it. The copy constructor, the assignment, Release and the destructor count the references of heap strings like the real ones
// (ObjectModel.h, ObjectModel.cpp), SetStringHandle takes over the handle without counting it like the real one (ObjectModel.h:158),
// and AppendAsString of a heap string appends the text the handle points to like the real one (ObjectModel.cpp:46-50)

#ifndef BINARY_CODE_FUZZ_OBJECTMODEL_H
#define BINARY_CODE_FUZZ_OBJECTMODEL_H

#include <BinaryShim.h>
#include <Platform/StringHandle.h>

struct ExpressionValue final
{
	uint32_t type : 8,
			 param : 24;
	union
	{
		bool bVal;
		char cVal;
		float fVal;
		int32_t iVal;
		uint32_t uVal;
		const char *_ecv_array sVal;
		StringHandle shVal;
		uintptr_t whole;								// uint32_t in the real one, which is as big as a pointer on the target
	};

	ExpressionValue() noexcept : type((uint32_t)TypeCode::None), param(0), whole(0) { }
	explicit ExpressionValue(int32_t i) noexcept : type((uint32_t)TypeCode::Int32), param(0), whole(0) { iVal = i; }
	explicit ExpressionValue(float f) noexcept : type((uint32_t)TypeCode::Float), param(1), whole(0) { fVal = f; }
	explicit ExpressionValue(const char *_ecv_array s) noexcept : type((uint32_t)TypeCode::CString), param(0), whole(0) { sVal = s; }
	explicit ExpressionValue(StringHandle h) noexcept : type((uint32_t)TypeCode::HeapString), param(0), shVal(h) { }

	ExpressionValue(const ExpressionValue& other) noexcept : type(other.type), param(other.param), whole(other.whole) { IncreaseRefCount(); }
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
	void SetInt(int32_t i) noexcept { Release(); type = (uint32_t)TypeCode::Int32; iVal = i; }
	void SetFloat(float f) noexcept { Release(); type = (uint32_t)TypeCode::Float; param = 1; fVal = f; }
	void SetStringHandle(StringHandle sh) noexcept { Release(); type = (uint32_t)TypeCode::HeapString; shVal = sh; }

	void AppendAsString(const StringRef& str, bool quoteStrings = false) const noexcept
	{
		switch (GetType())
		{
		case TypeCode::CString:		str.cat(sVal); break;
		case TypeCode::HeapString:	str.cat(shVal.Get().Ptr()); break;
		case TypeCode::Int32:		str.catf("%" PRIi32, iVal); break;
		case TypeCode::Uint32:		str.catf("%" PRIu32, uVal); break;
		case TypeCode::Float:		str.catf("%.1f", (double)fVal); break;
		default:					break;
		}
		(void)quoteStrings;
	}

	void Release() noexcept
	{
		if (GetType() == TypeCode::HeapString)
		{
			shVal.Delete();
			type = (uint32_t)TypeCode::None;
		}
	}

private:
	void IncreaseRefCount() noexcept
	{
		if (GetType() == TypeCode::HeapString)
		{
			shVal.IncreaseRefCount();
		}
	}
};

#endif
