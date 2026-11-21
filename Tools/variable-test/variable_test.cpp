// Host test of the values that RRF's variables keep: the real ObjectModel/Variable.cpp with the real Platform/Heap.cpp,
// Platform/StringHandle.cpp and Platform/ArrayHandle.cpp, compiled unchanged (ExpressionValue and the object model are the
// stand-ins in shim/ObjectModel/ObjectModel.h).
//
// var, global, set, macro parameters and the variables that DSF sets all end in Variable::Assign or Variable::AssignIndexed
// (StringParser::ProcessVarOrGlobalCommand and ProcessSetCommand, StringParser::AddParameters, SbcInterface SetVariable). A variable
// lives until its block ends or, for a global, until it is deleted, so it must not keep a pointer into the object model:
// - an object, e.g. heat.heaters[0] (ExpressionValue(const ObjectModel *), Heat.cpp OBJECT_MODEL_ARRAY_VALUE(self->heaters[...])),
//   which M950 H and M308 delete. Variable::Assign rejected a single object but copied an array of objects element by element.
// - an object model array, e.g. the inner arrays of heat.bedHeaterMapping or move.kinematics.forwardMatrix, which point at the
//   object that owns them (M669 deletes the kinematics in Move::SetKinematics)
// - a string of type CString, e.g. state.messageBox.message (MessageBox.cpp, self->message.c_str()), which M292 deletes, or
//   move.extruders[].filament (Move.cpp GetFilamentName), which M701 overwrites in place
// - a port, e.g. sensors.analog[0].port (SensorWithPort.cpp, self->port), which M308 deletes with the sensor
// The elements of heap arrays built in an expression ({...}, [...], ^, take, drop, vector) can be any of these too.

#include <ObjectModel/Variable.h>
#include <Platform/Heap.h>

#include <string>
#include <functional>

namespace Heap
{
	extern std::atomic<size_t> handlesUsed;					// defined in Heap.cpp, not declared in Heap.h
}

// RRF_ASSERT and the lock checks of the heap test shim
static unsigned int assertFailures = 0;

void HeapTestAssertFailed(uint32_t line, const char *file) noexcept
{
	++assertFailures;
	fprintf(stderr, "    RRF_ASSERT failed in %s line %u\n", file, (unsigned int)line);
}

void ReadWriteLock::CheckHasWriteLock() noexcept
{
	if (writers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasWriteLock");
	}
}

void ReadWriteLock::CheckHasReadLock() noexcept
{
	if (readers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasReadLock");
	}
}

void ReadWriteLock::CheckHasReadOrWriteLock() noexcept
{
	if (readers == 0 && writers == 0)
	{
		HeapTestAssertFailed(__LINE__, "CheckHasReadOrWriteLock");
	}
}

// Stand-ins for the object model items, with array tables built like those of the real classes

class FakeHeater : public ObjectModel
{
public:
	explicit FakeHeater(int n) noexcept : number(n) { }
	int number;
};

// heat: heaters as Heat.cpp array 2, bedHeaterMapping as Heat.cpp arrays 3 and 4 (the outer array returns the inner one without
// its own index, and the inner one reads it with context.GetIndex(1)), a 2D array whose inner elements are heaters, an array of
// two strings followed by a heater, and an array of strings
class FakeHeat : public ObjectModel
{
public:
	FakeHeater *heaters[2] = { nullptr, nullptr };
	size_t numSlots = 2;
	size_t slotCount[2] = { 1, 2 };
	int32_t mapping[2][2] = { { 1, 0 }, { 2, 3 } };
	FakeHeater *slotHeaters[2][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
	char names[2][16] = { "bed", "nozzle" };

protected:
	const ObjectModelArrayTableEntry *_ecv_null GetObjectModelArrayEntry(unsigned int index) const noexcept override;

private:
	static const ObjectModelArrayTableEntry table[];
};

#define HEAT(_x)	(static_cast<const FakeHeat *>(_x))

const ObjectModelArrayTableEntry FakeHeat::table[] =
{
	// 0. heaters
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return 2; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue((const ObjectModel *)HEAT(s)->heaters[c.GetLastIndex()]); } },
	// 1. bedHeaterMapping outer array
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return HEAT(s)->numSlots; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue(s, 2, true); } },
	// 2. bedHeaterMapping inner array
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext& c) noexcept -> size_t { return HEAT(s)->slotCount[c.GetLastIndex()]; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue(HEAT(s)->mapping[c.GetIndex(1)][c.GetLastIndex()]); } },
	// 3. heaters per slot, outer array
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return HEAT(s)->numSlots; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue(s, 4, true); } },
	// 4. heaters per slot, inner array
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext& c) noexcept -> size_t { return HEAT(s)->slotCount[c.GetLastIndex()]; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue((const ObjectModel *)HEAT(s)->slotHeaters[c.GetIndex(1)][c.GetLastIndex()]); } },
	// 5. two strings, then heater 0
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return 3; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept
	  {
		  return (c.GetLastIndex() < 2) ? ExpressionValue((const char *)HEAT(s)->names[c.GetLastIndex()]) : ExpressionValue((const ObjectModel *)HEAT(s)->heaters[0]);
	  } },
	// 6. strings
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return 2; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue((const char *)HEAT(s)->names[c.GetLastIndex()]); } },
};

const ObjectModelArrayTableEntry *_ecv_null FakeHeat::GetObjectModelArrayEntry(unsigned int index) const noexcept
{
	return (index < sizeof(table)/sizeof(table[0])) ? &table[index] : nullptr;
}

// tools[0]: heaters is an array of heater numbers
class FakeTool : public ObjectModel
{
public:
	int32_t heaterNumbers[2] = { 0, 1 };

protected:
	const ObjectModelArrayTableEntry *_ecv_null GetObjectModelArrayEntry(unsigned int index) const noexcept override
	{
		return (index == 0) ? &table[0] : nullptr;
	}

private:
	static const ObjectModelArrayTableEntry table[];
};

const ObjectModelArrayTableEntry FakeTool::table[] =
{
	{ nullptr,
	  [](const ObjectModel *s, const ObjectExplorationContext&) noexcept -> size_t { return 2; },
	  [](const ObjectModel *s, ObjectExplorationContext& c) noexcept { return ExpressionValue(static_cast<const FakeTool *>(s)->heaterNumbers[c.GetLastIndex()]); } },
};

struct FakeMessageBox { char message[40]; };			// MessageBox: String<MaxMessageLength> message, reported as message.c_str()
struct FakeFilament { char name[32]; };					// Filament: String<FilamentNameLength> name, reported as name.c_str()
struct FakeSensor { IoPort port{"temp0"}; };			// SensorWithPort: IoPort port, reported as ExpressionValue(port)

// Helpers

static const char *const ObjectError = "Cannot assign a value of type 'object' to a variable";

static std::string Describe(const ExpressionValue& v) noexcept
{
	switch (v.GetType())
	{
	case TypeCode::None:				return "null";
	case TypeCode::Int32:				return std::to_string(v.iVal);
	case TypeCode::HeapString:			return "\"" + std::string(v.shVal.Get().Ptr()) + "\"";
	case TypeCode::CString:				return "cstring";				// not dereferenced: it may point into freed memory
	case TypeCode::ObjectModel_tc:		return "object";
	case TypeCode::ObjectModelArray:	return "omarray";
	case TypeCode::Port_tc:				return "port";
	case TypeCode::HeapArray:
		{
			std::string s = "[";
			for (size_t i = 0; ; ++i)
			{
				ExpressionValue elem;
				{
					ReadLocker lock(Heap::heapLock);
					if (!v.ahVal.GetElement(i, elem))
					{
						break;
					}
				}
				if (i != 0)
				{
					s += ", ";
				}
				s += Describe(elem);
			}
			return s + "]";
		}
	default:							return "type " + std::to_string((unsigned int)v.type);
	}
}

// A heap array literal, built as ExpressionParser::ParseGeneralArray builds {e1, e2, ...}
static ExpressionValue Literal(std::initializer_list<ExpressionValue> elements) noexcept
{
	ArrayHandle ah;
	{
		WriteLocker locker(Heap::heapLock);
		ah.Allocate(elements.size());
		size_t i = 0;
		for (const ExpressionValue& e : elements)
		{
			ah.AssignElement(i++, const_cast<ExpressionValue&>(e));
		}
	}
	return ExpressionValue(ah);
}

// What 'var <name> = <expression>' does once the expression is parsed: VariableSet::InsertNew
static bool Insert(VariableSet& vars, const char *name, const ExpressionValue& ev, std::string& error) noexcept
{
	try
	{
		vars.InsertNew(name, ev, 0);
		return true;
	}
	catch (const GCodeException& e)
	{
		error = e.GetMessage();
		return false;
	}
}

static std::string ValueOf(VariableSet& vars, const char *name) noexcept
{
	const Variable *const v = vars.Lookup(name, false);
	return (v == nullptr) ? "(no variable)" : Describe(v->GetValue());
}

// Each case returns an empty string if it passes, else the reason
static unsigned int failures = 0, passes = 0;

static void Run(const char *name, std::function<std::string()> test) noexcept
{
	assertFailures = 0;
	const size_t handlesBefore = Heap::handlesUsed;
	std::string why = test();										// the variables of the case are deleted when it returns
	if (why.empty() && assertFailures != 0)
	{
		why = "RRF_ASSERT failed";
	}
	if (why.empty() && Heap::handlesUsed != handlesBefore)
	{
		why = std::to_string(Heap::handlesUsed - handlesBefore) + " heap slots still in use after the variables were deleted";
	}
	String<StringLength256> msg;
	if (why.empty() && !Heap::CheckIntegrity(msg.GetRef()))
	{
		why = std::string("heap damaged: ") + msg.c_str();
	}
	if (why.empty())
	{
		++passes;
		printf("PASS %s\n", name);
	}
	else
	{
		++failures;
		printf("FAIL %s: %s\n", name, why.c_str());
	}
}

static std::string ExpectRejected(VariableSet& vars, const char *name, const ExpressionValue& ev) noexcept
{
	std::string error;
	if (Insert(vars, name, ev, error))
	{
		return "accepted, the variable holds " + ValueOf(vars, name);
	}
	return (error == ObjectError) ? "" : "rejected with \"" + error + "\"";
}

int main()
{
	FakeHeater heater0(0), heater1(1);
	FakeTool tool;

	// K5: arrays of objects

	Run("var h = heat.heaters (array of objects) is rejected", [&]()
	{
		FakeHeat heat;
		heat.heaters[0] = &heater0;
		heat.heaters[1] = &heater1;
		VariableSet vars;
		return ExpectRejected(vars, "h", ExpressionValue(&heat, 0, true));
	});

	Run("var h = heat.heaters with no heater configured is an array of nulls", [&]()
	{
		FakeHeat heat;
		VariableSet vars;
		std::string error;
		if (!Insert(vars, "h", ExpressionValue(&heat, 0, true), error))
		{
			return "rejected with \"" + error + "\"";
		}
		const std::string v = ValueOf(vars, "h");
		return (v == "[null, null]") ? std::string() : "holds " + v;
	});

	Run("var m = 2D object model array whose inner elements are objects is rejected", [&]()
	{
		FakeHeat heat;
		heat.slotHeaters[1][1] = &heater1;
		VariableSet vars;
		return ExpectRejected(vars, "m", ExpressionValue(&heat, 3, true));
	});

	Run("var m = heat.bedHeaterMapping copies the inner arrays (also after the owner is deleted)", [&]()
	{
		FakeHeat *heat = new FakeHeat;
		VariableSet vars;
		std::string error;
		if (!Insert(vars, "m", ExpressionValue(heat, 1, true), error))
		{
			delete heat;
			return "rejected with \"" + error + "\"";
		}
		std::string v = ValueOf(vars, "m");						// Describe does not follow an object model array
		heat->mapping[1][1] = 7;
		delete heat;
		if (v != "[[1], [2, 3]]")
		{
			return "holds " + v + " instead of [[1], [2, 3]]";
		}
		v = ValueOf(vars, "m");
		return (v == "[[1], [2, 3]]") ? std::string() : "holds " + v + " after the change";
	});

	Run("var t = tools[0].heaters (array of ints) is copied", [&]()
	{
		VariableSet vars;
		std::string error;
		if (!Insert(vars, "t", ExpressionValue(&tool, 0, true), error))
		{
			return "rejected with \"" + error + "\"";
		}
		const std::string v = ValueOf(vars, "t");
		return (v == "[0, 1]") ? std::string() : "holds " + v;
	});

	Run("var a = {heat.heaters[0], 1} (heap array holding an object) is rejected", [&]()
	{
		VariableSet vars;
		return ExpectRejected(vars, "a", Literal({ ExpressionValue((const ObjectModel *)&heater0), ExpressionValue((int32_t)1) }));
	});

	Run("var a = {tools[0].heaters,} copies the inner array and leaves the literal unchanged", [&]()
	{
		VariableSet vars;
		const ExpressionValue literal = Literal({ ExpressionValue(&tool, 0, true) });
		std::string error;
		if (!Insert(vars, "a", literal, error))
		{
			return "rejected with \"" + error + "\"";
		}
		const std::string v = ValueOf(vars, "a");
		if (v != "[[0, 1]]")
		{
			return "holds " + v;
		}
		const std::string l = Describe(literal);
		return (l == "[omarray]") ? std::string() : "the literal changed to " + l;
	});

	Run("set var.x = heat.heaters is rejected and var.x keeps its value", [&]()
	{
		FakeHeat heat;
		heat.heaters[1] = &heater1;
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "x", ExpressionValue((int32_t)5), error);
		const size_t handlesBefore = Heap::handlesUsed;
		ExpressionValue ev(&heat, 0, true);
		try
		{
			vars.Lookup("x", false)->Assign(ev);
			return "accepted, the variable holds " + ValueOf(vars, "x");
		}
		catch (const GCodeException& e)
		{
			if (strcmp(e.GetMessage(), ObjectError) != 0)
			{
				return "rejected with \"" + std::string(e.GetMessage()) + "\"";
			}
		}
		if (Heap::handlesUsed != handlesBefore)
		{
			return std::to_string(Heap::handlesUsed - handlesBefore) + " heap slots left behind by the rejected assignment";
		}
		const std::string v = ValueOf(vars, "x");
		return (v == "5") ? std::string() : "holds " + v;
	});

	Run("set var.x[1] = heat.heaters[0] is rejected and var.x keeps its value", [&]()
	{
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "x", Literal({ ExpressionValue((int32_t)0), ExpressionValue((int32_t)0) }), error);
		const uint32_t index = 1;
		try
		{
			vars.Lookup("x", false)->AssignIndexed(ExpressionValue((const ObjectModel *)&heater0), 1, &index);
			return "accepted, the variable holds " + ValueOf(vars, "x");
		}
		catch (const GCodeException& e)
		{
			if (strcmp(e.GetMessage(), ObjectError) != 0)
			{
				return "rejected with \"" + std::string(e.GetMessage()) + "\"";
			}
		}
		const std::string v = ValueOf(vars, "x");
		return (v == "[0, 0]") ? std::string() : "holds " + v;
	});

	Run("set var.x[0] = tools[0].heaters stores a copy of the array", [&]()
	{
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "x", Literal({ ExpressionValue((int32_t)0), ExpressionValue((int32_t)0) }), error);
		const uint32_t index = 0;
		vars.Lookup("x", false)->AssignIndexed(ExpressionValue(&tool, 0, true), 1, &index);
		const std::string v = ValueOf(vars, "x");
		return (v == "[[0, 1], 0]") ? std::string() : "holds " + v;
	});

	Run("a rejected var leaves no heap slot behind: var h = heat.heaters[0]", [&]()
	{
		VariableSet vars;
		const size_t handlesBefore = Heap::handlesUsed;
		const std::string why = ExpectRejected(vars, "h", ExpressionValue((const ObjectModel *)&heater0));
		if (!why.empty())
		{
			return why;
		}
		return (Heap::handlesUsed == handlesBefore) ? std::string()
				: std::to_string(Heap::handlesUsed - handlesBefore) + " heap slot left behind (the name of the variable)";
	});

	Run("a rejected var leaves no heap slot behind: array of two strings and an object", [&]()
	{
		FakeHeat heat;
		heat.heaters[0] = &heater0;
		VariableSet vars;
		const size_t handlesBefore = Heap::handlesUsed;
		const std::string why = ExpectRejected(vars, "h", ExpressionValue(&heat, 5, true));
		if (!why.empty())
		{
			return why;
		}
		return (Heap::handlesUsed == handlesBefore) ? std::string()
				: std::to_string(Heap::handlesUsed - handlesBefore) + " heap slots left behind by the partly built array";
	});

	Run("set var.b = var.a shares the array instead of copying it", [&]()
	{
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "a", Literal({ ExpressionValue((int32_t)1), Literal({ ExpressionValue((int32_t)2) }) }), error);
		const size_t handlesBefore = Heap::handlesUsed;
		(void)Insert(vars, "b", vars.Lookup("a", false)->GetValue(), error);
		if (Heap::handlesUsed != handlesBefore + 1)				// the name of var.b
		{
			return std::to_string(Heap::handlesUsed - handlesBefore) + " heap slots allocated instead of 1";
		}
		const std::string v = ValueOf(vars, "b");
		return (v == "[1, [2]]") ? std::string() : "holds " + v;
	});

	// K6: strings and ports

	Run("var m = state.messageBox.message keeps its text after the box changes and is deleted", [&]()
	{
		FakeMessageBox *const box = new FakeMessageBox;
		strcpy(box->message, "Load filament");
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "m", ExpressionValue((const char *)box->message), error);
		strcpy(box->message, "Changed");
		const ExpressionValue v = vars.Lookup("m", false)->GetValue();
		const std::string why = (v.GetType() == TypeCode::CString)
									? std::string("holds a bare pointer into the message box, which now reads \"") + v.sVal + "\""
										: std::string();
		delete box;
		if (!why.empty())
		{
			return why;
		}
		const std::string s = ValueOf(vars, "m");
		return (s == "\"Load filament\"") ? std::string() : "holds " + s;
	});

	Run("var f = move.extruders[0].filament, then M701 loads another filament: the change is seen", [&]()
	{
		FakeFilament filament;
		strcpy(filament.name, "PLA");
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "fil_prev", ExpressionValue((const char *)filament.name), error);
		strcpy(filament.name, "PETG");
		const ExpressionValue v = vars.Lookup("fil_prev", false)->GetValue();
		const char *const stored = (v.GetType() == TypeCode::CString) ? v.sVal : (v.GetType() == TypeCode::HeapString) ? v.shVal.Get().Ptr() : "?";
		return (strcmp(stored, filament.name) != 0 && strcmp(stored, "PLA") == 0) ? std::string()
				: std::string("var.fil_prev reads \"") + stored + "\", so move.extruders[0].filament != var.fil_prev is false";
	});

	Run("var n = array of object model strings keeps its own copies", [&]()
	{
		FakeHeat heat;
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "n", ExpressionValue(&heat, 6, true), error);
		strcpy(heat.names[0], "chamber");
		const std::string v = ValueOf(vars, "n");
		return (v == "[\"bed\", \"nozzle\"]") ? std::string() : "holds " + v;
	});

	Run("var a = {state.messageBox.message,} keeps its own copy", [&]()
	{
		FakeMessageBox box;
		strcpy(box.message, "Load filament");
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "a", Literal({ ExpressionValue((const char *)box.message) }), error);
		strcpy(box.message, "Changed");
		const std::string v = ValueOf(vars, "a");
		return (v == "[\"Load filament\"]") ? std::string() : "holds " + v;
	});

	Run("var p = sensors.analog[0].port keeps the pin name after M308 deletes the sensor", [&]()
	{
		FakeSensor *const sensor = new FakeSensor;
		VariableSet vars;
		std::string error;
		(void)Insert(vars, "p", ExpressionValue(sensor->port), error);
		const bool bare = (vars.Lookup("p", false)->GetValue().GetType() == TypeCode::Port_tc);
		delete sensor;
		if (bare)
		{
			return std::string("holds a bare pointer to the port of the deleted sensor");
		}
		const std::string v = ValueOf(vars, "p");
		return (v == "\"temp0\"") ? std::string() : "holds " + v;
	});

	printf("%u passed, %u failed\n", passes, failures);
	return (failures == 0) ? 0 : 1;
}
