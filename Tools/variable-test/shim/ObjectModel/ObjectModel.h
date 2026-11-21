// Variable test stand-in for ObjectModel/ObjectModel.h: ExpressionValue, ObjectModel, ObjectModelArrayTableEntry,
// ObjectExplorationContext and IoPort as far as ObjectModel/Variable.cpp, Platform/ArrayHandle.cpp and the test use them.
// The constructors that the object model tables use, the copy constructor, the assignment, Release and the destructor
// behave like the real ones in ObjectModel.h and ObjectModel.cpp: a value of type CString, ObjectModel_tc,
// ObjectModelArray or Port_tc is a bare pointer, only heap strings and heap arrays count references.

#ifndef VARIABLE_TEST_OBJECTMODEL_H
#define VARIABLE_TEST_OBJECTMODEL_H

#include <RepRapFirmware.h>
#include <GCodes/GCodeException.h>
#include <Platform/StringHandle.h>
#include <Platform/ArrayHandle.h>
#include <ObjectModel/TypeCode.h>

class ObjectModel;
class ObjectModelArrayTableEntry;

constexpr size_t MaxExpressionArrayIndices = 5;		// Configuration.h

// IoPort: the pin name only (Hardware/IoPorts.h)
class IoPort
{
public:
	explicit IoPort(const char *name) noexcept { SafeStrncpy(pinName, name, sizeof(pinName)); }
	void AppendPinName(const StringRef& str) const noexcept { str.catf("%s", pinName); }

private:
	char pinName[20];
};

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
		const char *_ecv_array _ecv_null sVal;
		const ObjectModel *_ecv_from _ecv_null omVal;
		StringHandle shVal;
		ArrayHandle ahVal;
		const IoPort *iopVal;
		uintptr_t whole;								// uint32_t in the real one, which is as big as a pointer on the target
	};

	ExpressionValue() noexcept : type((uint32_t)TypeCode::None), param(0), whole(0) { }
	explicit ExpressionValue(int32_t i) noexcept : type((uint32_t)TypeCode::Int32), param(0), whole(0) { iVal = i; }
	explicit ExpressionValue(const ObjectModel *_ecv_from _ecv_null om) noexcept : type((om == nullptr) ? (uint32_t)TypeCode::None : (uint32_t)TypeCode::ObjectModel_tc), param(0), omVal(om) { }
	ExpressionValue(const ObjectModel *_ecv_from om, uint8_t arrayNumber, bool dummy) noexcept : type((uint32_t)TypeCode::ObjectModelArray), param(arrayNumber), omVal(om) { }
	explicit ExpressionValue(const char *_ecv_array _ecv_null s) noexcept : type((s == nullptr) ? (uint32_t)TypeCode::None : (uint32_t)TypeCode::CString), param(0), sVal(s) { }
	explicit ExpressionValue(StringHandle h) noexcept : type((uint32_t)TypeCode::HeapString), param(0), shVal(h) { }
	explicit ExpressionValue(ArrayHandle h) noexcept : type((uint32_t)TypeCode::HeapArray), param(0), ahVal(h) { }
	explicit ExpressionValue(const IoPort& p) noexcept : type((uint32_t)TypeCode::Port_tc), param(0), iopVal(&p) { }

	ExpressionValue(const ExpressionValue& other) noexcept
		: type(other.type), param(other.param), whole(other.whole)
	{
		IncreaseRefCount();
	}

	ExpressionValue(ExpressionValue&& other) noexcept
		: type(other.type), param(other.param), whole(other.whole)
	{
		other.type = (uint32_t)TypeCode::None;
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
	void SetStringHandle(StringHandle sh) noexcept { Release(); type = (uint32_t)TypeCode::HeapString; shVal = sh; }
	void SetArrayHandle(ArrayHandle ah) noexcept { Release(); type = (uint32_t)TypeCode::HeapArray; ahVal = ah; }
	void SetNull(std::nullptr_t dummy) noexcept { Release(); type = (uint32_t)TypeCode::None; }

	// The real one handles every type; Variable.cpp uses it for ports, as ExpressionParser::ConvertToString does
	void AppendAsString(const StringRef& str, bool quoteStrings = false) const noexcept
	{
		switch (GetType())
		{
		case TypeCode::CString:		str.catf("%s", sVal); break;
		case TypeCode::HeapString:	str.catf("%s", shVal.Get().Ptr()); break;
		case TypeCode::Int32:		str.catf("%d", (int)iVal); break;
		case TypeCode::Port_tc:		iopVal->AppendPinName(str); break;
		default:					str.catf("(type %u)", (unsigned int)type); break;
		}
	}

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

// ObjectExplorationContext: the array indices, with the members of ObjectModel.cpp that use them
class ObjectExplorationContext
{
public:
	ObjectExplorationContext() noexcept : numIndicesCounted(0) { }

	void AddIndex(int32_t index) THROWS(GCodeException)
	{
		if (numIndicesCounted == MaxExpressionArrayIndices)
		{
			throw GCodeException("Too many indices");
		}
		indices[numIndicesCounted] = index;
		++numIndicesCounted;
	}

	void RemoveIndex() THROWS(GCodeException)
	{
		if (numIndicesCounted == 0)
		{
			throw GCodeException("internal error");
		}
		--numIndicesCounted;
	}

	int32_t GetIndex(size_t n) const noexcept { return (n < numIndicesCounted) ? indices[numIndicesCounted - n - 1] : 0; }
	int32_t GetLastIndex() const noexcept { return (numIndicesCounted != 0) ? indices[numIndicesCounted - 1] : 0; }
	size_t GetNumIndicesCounted() const noexcept { return numIndicesCounted; }

private:
	size_t numIndicesCounted;
	int32_t indices[MaxExpressionArrayIndices];
};

class ObjectModelArrayTableEntry
{
public:
	ReadWriteLock *null lockPointer;
	size_t (*GetNumElements)(const ObjectModel *_ecv_from, const ObjectExplorationContext&) noexcept;
	ExpressionValue (*GetElement)(const ObjectModel *_ecv_from, ObjectExplorationContext&) noexcept;
};

class ObjectModel
{
public:
	ObjectModel() noexcept { }
	virtual ~ObjectModel() { }

	const ObjectModelArrayTableEntry *_ecv_null FindObjectModelArrayEntry(unsigned int index) const noexcept { return GetObjectModelArrayEntry(index); }

protected:
	virtual const ObjectModelArrayTableEntry *_ecv_null GetObjectModelArrayEntry(unsigned int index) const noexcept { return nullptr; }
};

#endif
