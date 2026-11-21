/*
 * Variable.cpp
 *
 *  Created on: 6 Mar 2021
 *      Author: David
 */

#include "Variable.h"
#include <Platform/OutputMemory.h>
#include <GCodes/GCodeBuffer/GCodeBuffer.h>

// Members of class Variable
Variable::Variable(const char *_ecv_array str, size_t strLen, ExpressionValue& pVal, int16_t pScope) THROWS(GCodeException)
	: name(str, strLen), val(), scope(pScope)
{
	try
	{
		Assign(pVal);			// this may throw
	}
	catch (...)
	{
		name.Delete();			// the destructor isn't called if the constructor throws, so release the name here
		throw;
	}
}

Variable::~Variable()
{
	name.Delete();
	val.Release();
}

bool Variable::IsValidVariableName(const char *_ecv_array str) noexcept
{
	size_t len = strlen(str);
	if (len == 0)
	{
		return false;
	}

	if (!isAlpha(str[0]))
	{
		return false;
	}

	for (size_t i = 1; i < len; i++)
	{
		if (!isAlnum(str[i]) && str[i] != '_')
		{
			return false;
		}
	}

	if (strcmp(str, "result") == 0)
	{
		return false;
	}
	return true;
}

// Assign a new value to this variable
void Variable::Assign(ExpressionValue& ev) THROWS(GCodeException)
{
	ExpressionValue newVal(ev);
	(void)MakeStorable(newVal);											// this may throw, in which case the variable keeps its old value
	val = newVal;
}

// Assign a new value to an indexed part of this variable. There is always at least one index.
void Variable::AssignIndexed(const ExpressionValue& ev, size_t numIndices, const uint32_t *_ecv_array indices) THROWS(GCodeException)
{
	if (val.GetType() != TypeCode::HeapArray)
	{
		throw GCodeException("Expected an array expression");
	}
	ExpressionValue newVal(ev);
	(void)MakeStorable(newVal);
	val.ahVal.AssignIndexed(newVal, numIndices, indices);
}

// Convert a value to a form that a variable can keep after the object model has changed or deleted the item that the value came from.
// Object model arrays are copied to the heap, strings and ports are replaced by heap strings, and the elements of heap arrays are converted in the same way,
// because they may have come from the object model too, e.g. {heat.heaters[0], 1} or take(heat.heaters, 2).
// Objects can't be copied, so they are rejected wherever they occur, e.g. heat.heaters.
// Return true if the value was replaced.
/*static*/ bool Variable::MakeStorable(ExpressionValue& ev) THROWS(GCodeException)
{
	switch (ev.GetType())
	{
	case TypeCode::ObjectModel_tc:
		throw GCodeException("Cannot assign a value of type 'object' to a variable");

	case TypeCode::ObjectModelArray:
		CopyObjectModelArray(ev);
		return true;

	case TypeCode::HeapArray:
		{
			bool changed = false;
			for (uint32_t i = 0; ; ++i)
			{
				ExpressionValue elemVal;
				{
					ReadLocker lock(Heap::heapLock);						// must have a read lock on heapLock when calling GetNumElements or GetElement
					if (!ev.ahVal.GetElement(i, elemVal))
					{
						break;
					}
				}
				if (MakeStorable(elemVal))
				{
					ev.ahVal.AssignIndexed(elemVal, 1, &i);					// this copies the array first if it is shared, so the original stays as it was
					changed = true;
				}
			}
			return changed;
		}

	case TypeCode::CString:
		{
			// The string may belong to an object model item that changes or deletes it later, e.g. state.messageBox.message
			const StringHandle sh(ev.sVal);
			ev.SetStringHandle(sh);
		}
		return true;

	case TypeCode::Port_tc:
		StorePortName(ev);
		return true;

	default:
		return false;
	}
}

// Replace an object model array value by a copy of the array on the heap.
// This is a separate function to keep the exploration context off the stack of the recursive function MakeStorable.
/*static*/ void Variable::CopyObjectModelArray(ExpressionValue& ev) THROWS(GCodeException)
{
	const ObjectModelArrayTableEntry *const entry = ev.omVal->FindObjectModelArrayEntry(ev.param & 0xFF);
	if (entry == nullptr)
	{
		throw GCodeException("Failed to lookup object model array");
	}
	ReadLocker lock(entry->lockPointer);
	ObjectExplorationContext context;
	context.AddIndex(ev.param >> 8);										// in case it is a 2D array
	const size_t numElements = entry->GetNumElements(ev.omVal, context);
	ArrayHandle ah;
	if (numElements != 0)
	{
		WriteLocker locker(Heap::heapLock);									// prevent other tasks modifying the heap
		ah.Allocate(numElements);
	}
	ExpressionValue copy(ah);												// from here on 'copy' owns the array, so it is released if an element is rejected
	for (size_t i = 0; i < numElements; ++i)
	{
		context.AddIndex(i);
		ExpressionValue elemVal = entry->GetElement(ev.omVal, context);
		context.RemoveIndex();
		if (elemVal.GetType() == TypeCode::ObjectModelArray)
		{
			elemVal.param |= (uint32_t)i << 8;								// the inner array needs our index, as in ObjectModel::GetObjectValue
		}
		(void)MakeStorable(elemVal);										// this may throw
		WriteLocker locker(Heap::heapLock);									// prevent other tasks modifying the heap
		copy.ahVal.AssignElement(i, elemVal);
	}
	ev = copy;
}

// Replace a port value by the name of the port, as ExpressionParser::ConvertToString does. The port belongs to an object that may be deleted later,
// e.g. sensors.analog[0].port. This is a separate function to keep the string buffer off the stack of the recursive function MakeStorable.
/*static*/ void Variable::StorePortName(ExpressionValue& ev) noexcept
{
	String<MaxStringExpressionLength> str;
	ev.AppendAsString(str.GetRef());
	const StringHandle sh(str.c_str());
	ev.SetStringHandle(sh);
}

// Create a new array and assign it to this variable
void Variable::AssignArray(size_t numElements, function_ref<ExpressionValue(size_t)> func) noexcept
{
	ArrayHandle ah;
	WriteLocker locker(Heap::heapLock);						// prevent other tasks modifying the heap
	ah.Allocate(numElements);
	for (size_t i = 0; i < numElements; ++i)
	{
		ExpressionValue elem = func(i);
		ah.AssignElement(i, elem);
	}
	val.SetArrayHandle(ah);
}

// Members of class VariableSet

// Hash a variable name (FNV-1a). Stop at a null character like the copy of the name that the variable stores.
uint32_t VariableSet::HashName(const char *_ecv_array str, size_t strLen) noexcept
{
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < strLen && str[i] != 0; ++i)
	{
		hash = (hash ^ (uint8_t)str[i]) * 16777619u;
	}
	return hash;
}

Variable *_ecv_null VariableSet::Lookup(const char *_ecv_array str, bool wantParameter) noexcept
{
	const uint32_t hash = HashName(str, strlen(str));
	LinkedVariable *lv;
	for (lv = root; lv != nullptr; lv = lv->next)
	{
		if (lv->nameHash == hash && (wantParameter == (lv->v.GetScope() == -1)))
		{
			auto vname = lv->v.GetName();
			if (strcmp(vname.Ptr(), str) == 0)
			{
				return &(lv->v);
			}
		}
	}
	return nullptr;
}

const Variable *_ecv_null VariableSet::Lookup(const char *_ecv_array str, size_t length, bool wantParameter) const noexcept
{
	const uint32_t hash = HashName(str, length);
	const LinkedVariable *lv;
	for (lv = root; lv != nullptr; lv = lv->next)
	{
		if (lv->nameHash == hash && (wantParameter == (lv->v.GetScope() == -1)))
		{
			auto vname = lv->v.GetName();
			if (strlen(vname.Ptr()) == length && memcmp(vname.Ptr(), str, length) == 0)
			{
				return &(lv->v);
			}
		}
	}
	return nullptr;
}

Variable *VariableSet::InsertNew(const char *_ecv_array str, ExpressionValue pVal, int16_t pScope) THROWS(GCodeException)
{
	return InsertNew(str, strlen(str), pVal, pScope);
}

Variable *VariableSet::InsertNew(const char *_ecv_array str, size_t strLen, ExpressionValue pVal, int16_t pScope) THROWS(GCodeException)
{
	LinkedVariable * const toInsert = new LinkedVariable(str, strLen, pVal, pScope, root);
	root = toInsert;
	return &(toInsert->v);
}

// Remove all variables with a scope greater than the parameter
void VariableSet::EndScope(uint8_t blockNesting) noexcept
{
	LinkedVariable *prev = nullptr;
	for (LinkedVariable *lv = root; lv != nullptr; )
	{
		if (lv->v.GetScope() > (int16_t)blockNesting)
		{
			LinkedVariable *temp = lv;
			lv = lv->next;
			if (prev == nullptr)
			{
				root = lv;
			}
			else
			{
				prev->next = lv;
			}
			delete temp;
		}
		else
		{
			prev = lv;
			lv = lv->next;
		}
	}
}

void VariableSet::Delete(const char *_ecv_array str) noexcept
{
	LinkedVariable *prev = nullptr;
	for (LinkedVariable *lv = root; lv != nullptr; lv = lv->next)
	{
		auto vname = lv->v.GetName();
		if (strcmp(vname.Ptr(), str) == 0)
		{
			if (prev == nullptr)
			{
				root = lv->next;
			}
			else
			{
				prev->next = lv->next;
			}
			delete lv;
			break;
		}
		prev = lv;
	}
}

void VariableSet::Clear() noexcept
{
	while (root != nullptr)
	{
		LinkedVariable *lv = root;
		root = lv->next;
		delete lv;
	}
}

VariableSet::~VariableSet()
{
	Clear();
}

// Delete any existing variables and instead use the variables from the argument, taking ownership of them
void VariableSet::AssignFrom(VariableSet& other) noexcept
{
	Clear();
	root = other.root;
	other.root = nullptr;
}

void VariableSet::IterateWhile(function_ref_noexcept<bool(unsigned int, const Variable&) noexcept> func) const noexcept
{
	unsigned int num = 0;
	for (const LinkedVariable *lv = root; lv != nullptr; lv = lv->next)
	{
		if (!func(num, lv->v))
		{
			break;
		}
		++num;
	}
}

// End
