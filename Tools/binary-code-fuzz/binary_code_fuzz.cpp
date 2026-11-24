// Fuzz target for the binary G-codes that DSF sends to RRF in SBC mode, from the moment the code is in the buffer of its G-code buffer
// (GCodeBuffer::PutBinary -> BinaryParser::Put) until GCodes has read every parameter of it.
//
// Real code:
// - src/GCodes/GCodeBuffer/BinaryParser.cpp, compiled unchanged (a copy in gen/ so that its "GCodeBuffer.h" and "ExpressionParser.h"
//   includes find the stand-ins in shim/, see the Makefile): every function of BinaryParser
// - src/Platform/Heap.cpp and src/Platform/StringHandle.cpp, compiled unchanged: the string heap that AddParameters stores string
//   parameters in
// - GCodeException::GetMessage and ThrowGCodeException, copied out of src/GCodes/GCodeException.cpp at build time
//   (gen/GCodeException.inc); DriverId and GCodeBufferState, copied out of src/RepRapFirmware.h and GCodeBuffer.h (gen/RrfTypes.inc)
// - StringRef, String<N>, SafeStrncpy, StrToU32, StrHexToU32, IPAddress and the other string functions of RRFLibraries, compiled
//   unchanged
//
// Played here (not RRF): the G-code buffer (shim/GCodeBuffer.h), the expression parser (shim/ExpressionParser.h), ExpressionValue and
// VariableSet (shim/ObjectModel), the GCodeException constructors that take a G-code buffer (they add the file name and line of a
// file macro, which the fuzz buffer never runs), Tasks::AllocPermanent and the RTOS locks.
//
// Input: byte 0 selects what GCodes does with the code (bit 0: Gcodes debugging on, so DecodeCommand prints the code; bit 1: "Bad
// command" reply with AppendFullCommand; bit 2: PrintCommand; bit 3: GetUnprecedentedString; bit 4: allowEmpty for it; bit 5:
// AddParameters, as for a macro call; bit 6: codeRunning 98 for it (M98 skips P); bit 7: ContainsExpression, GetFilePosition and
// SetFilePosition), byte 1 the first getter that GCodes calls after Seen (each further parameter takes the next one), byte 2 the
// array length that GCodes passes to the array getters (1 to 16), byte 3 the byte that fills the code buffer before the code is put
// (what earlier codes left there on the board). Then the code as the Code packet carries it: SbcInterface accepts only 20 to 384
// bytes in a multiple of 4 (SbcInterface.cpp:396), so other lengths do not run.

#include <cstdlib>
#include <vector>

#include <BinaryShim.h>
#include <GCodeBuffer.h>
#include <ExpressionParser.h>
#include <ObjectModel/Variable.h>
#include <Networking/NetworkDefs.h>
#include <GCodes/GCodeException.h>
#if defined(__SANITIZE_ADDRESS__)
# define FUZZ_ASAN	1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define FUZZ_ASAN	1
# endif
#endif
#ifdef FUZZ_ASAN
# include <sanitizer/asan_interface.h>
# include <sanitizer/common_interface_defs.h>
#else
# define ASAN_POISON_MEMORY_REGION(a, s)	((void)(a), (void)(s))
# define ASAN_UNPOISON_MEMORY_REGION(a, s)	((void)(a), (void)(s))
#endif

namespace
{
	bool verbose = false;
	GCodeBuffer *gb = nullptr;
}

RepRap reprap;

extern "C" void debugPrintf(const char *fmt, ...) noexcept
{
	if (!verbose) { return; }
	va_list vargs;
	va_start(vargs, fmt);
	vfprintf(stderr, fmt, vargs);
	va_end(vargs);
}

[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept
{
	fprintf(stderr, "RRF_ASSERT failed at %s:%u, the board would reset\nSUMMARY: RRF_ASSERT %s:%u\n", file, (unsigned int)line, file, (unsigned int)line);
	abort();
}

// Every permanent allocation is a heap block of its own. The code buffer of the G-code buffer (BinaryParser::AllocateBuffer) and the
// index and storage blocks of the string heap (Heap.cpp) come from here
void *Tasks::AllocPermanent(size_t sz, std::align_val_t align) noexcept
{
	return ::operator new(sz, align);
}

// GCodeException.cpp: the constructors that take a G-code buffer add its file name and line number when it runs a file or file macro
// (GCodeException.cpp:15-37); the fuzz buffer runs neither, so they are the constructors without a buffer
GCodeException::GCodeException(const GCodeBuffer *_ecv_null, int col, GCodeExceptionMessage msg) noexcept : line(-1), column(col), message(msg.GetText()) { }
GCodeException::GCodeException(const GCodeBuffer *_ecv_null p_gb, int col, GCodeExceptionMessage msg, uint32_t uparam) noexcept : GCodeException(p_gb, col, msg)
{
	param.u = uparam;
}
GCodeException::GCodeException(const GCodeBuffer *_ecv_null p_gb, int col, GCodeExceptionMessage msg, const char *_ecv_array sparam) noexcept : GCodeException(p_gb, col, msg)
{
	stringParam.copy(sparam);
}

#include "GCodeException.inc"

// ---------------------------------------------------------------------------------------------------------------------
// The stand-in expression parser, see shim/ExpressionParser.h

int32_t ExpressionParser::Number() THROWS(GCodeException)
{
	const char c = CurrentCharacter();
	if (c >= '0' && c <= '9') { return c - '0'; }
	if (c == '-') { return -1; }
	throw GCodeException(-1, -1, "fuzz stand-in: expected a number");
}

ExpressionValue ExpressionParser::Parse(bool) THROWS(GCodeException)
{
	switch (CurrentCharacter())
	{
	case '"':	return ExpressionValue("fuzz");
	case 'h':	return ExpressionValue(StringHandle("fuzz"));
	case 'f':	return ExpressionValue(1.5f);
	default:	return ExpressionValue(Number());
	}
}

float ExpressionParser::ParseFloat() THROWS(GCodeException) { return (CurrentCharacter() == 'f') ? 1.5f : (float)Number(); }
int32_t ExpressionParser::ParseInteger() THROWS(GCodeException) { return Number(); }

uint32_t ExpressionParser::ParseUnsigned() THROWS(GCodeException)
{
	const int32_t n = Number();
	if (n < 0) { throw GCodeException(-1, -1, "fuzz stand-in: expected a non-negative number"); }
	return (uint32_t)n;
}

DriverId ExpressionParser::ParseDriverId() THROWS(GCodeException) { return DriverId(0, (uint8_t)ParseUnsigned()); }

void ExpressionParser::ParseFloatArray(float arr[], size_t& length) THROWS(GCodeException)
{
	if (length == 0) { throw GCodeException(-1, -1, "fuzz stand-in: array too long"); }
	arr[0] = ParseFloat();
	length = 1;
}

void ExpressionParser::ParseIntArray(int32_t arr[], size_t& length) THROWS(GCodeException)
{
	if (length == 0) { throw GCodeException(-1, -1, "fuzz stand-in: array too long"); }
	arr[0] = ParseInteger();
	length = 1;
}

void ExpressionParser::ParseUnsignedArray(uint32_t arr[], size_t& length) THROWS(GCodeException)
{
	if (length == 0) { throw GCodeException(-1, -1, "fuzz stand-in: array too long"); }
	arr[0] = ParseUnsigned();
	length = 1;
}

void ExpressionParser::ParseDriverIdArray(DriverId arr[], size_t& length) THROWS(GCodeException)
{
	if (length == 0) { throw GCodeException(-1, -1, "fuzz stand-in: array too long"); }
	arr[0] = ParseDriverId();
	length = 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// VariableSet: keeps a copy of each parameter and reads it back as a macro reads param.X. A heap string must end within its storage
// space (Heap.h:22-26: a 16-bit length including the length field, then the text and its null), which StringHandle::InternalAssign
// makes sure of for every length that fits (StringHandle.cpp:66-73); the check reports a string that does not, because reading it
// runs on into the next storage spaces of the heap block, which AddressSanitizer cannot see as long as it stays in the 2 KB block

namespace
{
	void CheckHeapString(const ExpressionValue& ev) noexcept
	{
		if (ev.GetType() != TypeCode::HeapString || ev.shVal.IsNull()) { return; }		// a null handle is the empty string (StringHandle.cpp:101)
		const ReadLockedPointer<const char> p = ev.shVal.Get();
		uint16_t spaceLength;
		memcpy(&spaceLength, p.Ptr() - sizeof(uint16_t), sizeof(uint16_t));
		spaceLength &= ~1u;											// Heap.cpp: bit 0 marks a released space
		const size_t textSpace = (spaceLength >= sizeof(uint16_t)) ? spaceLength - sizeof(uint16_t) : 0;
		if (textSpace == 0 || memchr(p.Ptr(), 0, textSpace) == nullptr)
		{
			fprintf(stderr, "ERROR: FUZZ: heap string without a null in its storage space of %u bytes (StringHandle::InternalAssign)\n",
					(unsigned int)spaceLength);
#ifdef FUZZ_ASAN
			__sanitizer_print_stack_trace();
#endif
			fprintf(stderr, "SUMMARY: FUZZ: unterminated-heap-string\n");
			abort();
		}
	}
}

void VariableSet::InsertNewParameter(const char *_ecv_array str, ExpressionValue pVal) THROWS(GCodeException)
{
	CheckHeapString(pVal);
	if (count < MaxValues)
	{
		names[count][0] = str[0];
		names[count][1] = 0;
		values[count] = pVal;
		++count;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The target

namespace
{
	constexpr size_t ConfigBytes = 4;
	constexpr unsigned int NumGetters = 13;

	template<class T> void ArrayGetter(BinaryParser& bp, size_t arrayLength, void (BinaryParser::*getter)(T[], size_t&)) THROWS(GCodeException)
	{
		std::vector<T> arr(arrayLength);					// exactly as long as GCodes says, so that a write beyond it is seen
		size_t length = arrayLength;
		(bp.*getter)(arr.data(), length);
	}

	// What GCodes does with a parameter after gb.Seen(letter), one getter per call
	void CallGetter(BinaryParser& bp, unsigned int getter, size_t arrayLength) THROWS(GCodeException)
	{
		switch (getter % NumGetters)
		{
		case 0:		(void)bp.GetFValue(); break;
		case 1:		(void)bp.GetIValue(); break;
		case 2:		(void)bp.GetUIValue(); break;
		case 3:		(void)bp.GetDriverId(); break;
		case 4:		{ IPAddress ip; bp.GetIPAddress(ip); } break;
		case 5:		{ MacAddress mac; bp.GetMacAddress(mac); } break;
		case 6:		{ String<MaxFilenameLength> s; bp.GetPossiblyQuotedString(s.GetRef(), (arrayLength & 1) != 0); } break;
		case 7:		ArrayGetter<float>(bp, arrayLength, &BinaryParser::GetFloatArray); break;
		case 8:		ArrayGetter<int32_t>(bp, arrayLength, &BinaryParser::GetIntArray); break;
		case 9:		ArrayGetter<uint32_t>(bp, arrayLength, &BinaryParser::GetUnsignedArray); break;
		case 10:	ArrayGetter<DriverId>(bp, arrayLength, &BinaryParser::GetDriverIdArray); break;
		case 11:	{ const ExpressionValue ev = bp.GetExpression(); String<StringLength100> s; ev.AppendAsString(s.GetRef()); } break;
		default:	{ uint32_t u; String<StringLength100> s; (void)bp.GetStringOrUIValue(u, s.GetRef()); } break;
		}
	}

	// GCodes reports the exception with its message (GCodes::HandleReply after GetMessage)
	void ReportException(const GCodeException& e) noexcept
	{
		String<StringLength256> reply;
		e.GetMessage(reply.GetRef(), gb);
		if (verbose) { fprintf(stderr, "exception: %s\n", reply.c_str()); }
	}
}

// The parameter layout that BinaryParser::Put must accept: the parameter table, then the value of each parameter in table order, as DSF
// writes them (Writer.WriteCode, Writer.cs) and BinaryParser::WriteParameters steps over them (BinaryParser.cpp): intValue elements of
// 4 bytes for the array types, intValue bytes padded to a multiple of 4 for String, Expression and DateTime, intValue bytes for
// BoolArray, 8 bytes for ULong and Bitmap64, nothing for the others. The code fits if every value ends within it and no element count is
// negative. 64-bit arithmetic, written independently of BinaryParser::ParametersFit, which the target checks against it
static bool CodeFitsModel(const uint8_t *code, size_t length) noexcept
{
	CodeHeader h;
	memcpy(&h, code, sizeof(h));
	int64_t end = (int64_t)sizeof(CodeHeader) + (int64_t)h.numParameters * (int64_t)sizeof(CodeParameter);
	if (end > (int64_t)length) { return false; }
	for (unsigned int i = 0; i < h.numParameters; ++i)
	{
		CodeParameter p;
		memcpy(&p, code + sizeof(CodeHeader) + i * sizeof(CodeParameter), sizeof(p));
		const int64_t n = p.intValue;
		switch (p.type)
		{
		case DataType::IntArray: case DataType::UIntArray: case DataType::FloatArray: case DataType::DriverIdArray:
			if (n < 0) { return false; }
			end += 4 * n;
			break;
		case DataType::String: case DataType::Expression: case DataType::DateTime:
			if (n < 0) { return false; }
			end += (n + 3) / 4 * 4;
			break;
		case DataType::BoolArray:
			if (n < 0) { return false; }
			end += n;
			break;
		case DataType::ULong: case DataType::Bitmap64:
			end += 8;
			break;
		default:
			break;
		}
		if (end > (int64_t)length) { return false; }
	}
	return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < ConfigBytes + sizeof(CodeHeader) || size - ConfigBytes > MaxGCodeBinaryLength || ((size - ConfigBytes) % sizeof(uint32_t)) != 0)
	{
		return 0;
	}
	const uint8_t config = data[0], firstGetter = data[1], fill = data[3];
	const size_t arrayLength = 1 + data[2] % 16;
	const size_t codeLength = size - ConfigBytes;

	if (gb == nullptr)
	{
		gb = new GCodeBuffer(GCodeChannel::SBC);
		gb->binaryParser.Init();
		gb->binaryParser.AllocateBuffer();			// as GCodeBuffer::PrimeBinaryBuffer does before the first code, so that every input is filled
	}

	// What earlier codes left in the buffer, then the code as GCodeBuffer::PutBinary puts it (FillBuffer passes the words of the code)
	ASAN_UNPOISON_MEMORY_REGION(gb->buffer, MaxGCodeBinaryLength);
	memset(gb->buffer, fill, MaxGCodeBinaryLength);
	std::vector<uint32_t> words(codeLength / sizeof(uint32_t));
	memcpy(words.data(), data + ConfigBytes, codeLength);
	gb->binaryParser.Put(words.data(), words.size());
	const bool accepted = (gb->binaryParser.DataLength() != 0);
	if (accepted != CodeFitsModel(data + ConfigBytes, codeLength))
	{
		// BinaryParser::Put stores a code whose parameters do not fit in it, or refuses one whose parameters fit
		fprintf(stderr, "ERROR: FUZZ: BinaryParser::Put %s a code of %u bytes that %s\nSUMMARY: FUZZ: parameter-check-differs\n",
				(accepted) ? "stored" : "refused", (unsigned int)codeLength, (accepted) ? "does not fit" : "fits");
		abort();
	}
	if (gb->overflowed || gb->malformed != !accepted)
	{
		fprintf(stderr, "ERROR: FUZZ: overflow %d and malformed %d after BinaryParser::Put of a code of %u bytes\nSUMMARY: FUZZ: put-flags-wrong\n",
				(int)gb->overflowed, (int)gb->malformed, (unsigned int)codeLength);
		abort();
	}
	if (getenv("FUZZ_POISON_STALE") != nullptr)
	{
		// Report reads of what earlier codes left behind the code, too (wrong data, not a memory error)
		ASAN_POISON_MEMORY_REGION(gb->buffer + codeLength, MaxGCodeBinaryLength - codeLength);
	}

	reprap.debugFlags.Clear();
	if ((config & 0x01) != 0) { reprap.debugFlags.SetBit(gb->GetChannel().ToBaseType()); }
	gb->binaryParser.DecodeCommand();
	BinaryParser& bp = gb->binaryParser;

	if ((config & 0x02) != 0)
	{
		// GCodes2.cpp:166, "Bad command: " and the code; M122 prints the code of every busy channel the same way (GCodeBuffer.cpp:242-255)
		String<StringLength256> reply;
		reply.copy("Bad command: ");
		bp.AppendFullCommand(reply.GetRef());
	}
	if ((config & 0x04) != 0)
	{
		String<StringLength20> s;
		bp.PrintCommand(s.GetRef());
	}
	if ((config & 0x08) != 0)
	{
		try
		{
			String<StringLength256> s;
			bp.GetUnprecedentedString(s.GetRef(), (config & 0x10) != 0);
		}
		catch (const GCodeException& e) { ReportException(e); }
	}

	// Every parameter letter of the code, as the code handlers ask for them
	unsigned int getter = firstGetter;
	const ParameterLettersBitmap letters = bp.AllParameters();
	for (unsigned int bit = 0; bit < 52; ++bit)
	{
		if (!letters.IsBitSet(bit)) { continue; }
		const char letter = (bit < 26) ? (char)('A' + bit) : (char)('a' + (bit - 26));
		if (bp.Seen(letter))
		{
			try { CallGetter(bp, getter++, arrayLength); }
			catch (const GCodeException& e) { ReportException(e); }
		}
	}

	if ((config & 0x20) != 0)
	{
		// GCodes::DoFileMacroWithParameters (GCodes.cpp:3718), then the macro reads its parameters
		VariableSet vars;
		try
		{
			bp.AddParameters(vars, ((config & 0x40) != 0) ? 98 : bp.GetCommandNumber());
		}
		catch (const GCodeException& e) { ReportException(e); }
		for (unsigned int i = 0; i < vars.count; ++i)
		{
			String<StringLength256> s;
			vars.values[i].AppendAsString(s.GetRef());
		}
	}
	if ((config & 0x80) != 0)
	{
		(void)bp.ContainsExpression();
		const FilePosition fpos = bp.GetFilePosition();
		bp.SetFilePosition((fpos == noFilePosition) ? 0x1234 : noFilePosition);
	}
	bp.SetFinished();
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Seeds: codes as DSF's Writer.WriteCode encodes them (src/DuetControlServer/Link/Protocol/Writer.cs), each with a few configurations

constexpr size_t FuzzMaxInputSize = ConfigBytes + MaxGCodeBinaryLength;

namespace
{
	typedef std::vector<uint8_t> Bytes;

	struct Param
	{
		char letter;
		DataType type;
		int32_t intValue;
		Bytes data;								// the array elements or string bytes, without the padding
	};

	template<class T> void Append(Bytes& b, const T& v)
	{
		const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
		b.insert(b.end(), p, p + sizeof(T));
	}

	Param IntParam(char letter, int32_t v) { return Param{ letter, DataType::Int, v, {} }; }
	Param UIntParam(char letter, uint32_t v) { return Param{ letter, DataType::UInt, (int32_t)v, {} }; }
	Param DriverParam(char letter, uint32_t board, uint32_t driver) { return Param{ letter, DataType::DriverId_dt, (int32_t)((board << 16) | driver), {} }; }
	Param FloatParam(char letter, float f) { int32_t v; memcpy(&v, &f, 4); return Param{ letter, DataType::Float, v, {} }; }
	Param StringParam(char letter, const char *s, DataType type = DataType::String)
	{
		return Param{ letter, type, (int32_t)strlen(s), Bytes(s, s + strlen(s)) };
	}
	template<class T> Param ArrayParam(char letter, DataType type, std::initializer_list<T> values)
	{
		Param p{ letter, type, (int32_t)values.size(), {} };
		for (T v : values) { Append(p.data, v); }
		return p;
	}
	Param NullParam(char letter) { return Param{ letter, DataType::Null, 0, {} }; }

	Bytes Code(uint8_t config, uint8_t getter, uint8_t arrayLength, char letter, int32_t major, int32_t minor, uint8_t flags, std::initializer_list<Param> params)
	{
		Bytes b = { config, getter, arrayLength, 0x41 };
		CodeHeader h;
		memset(&h, 0, sizeof(h));
		h.channel = GCodeChannel::SBC;
		h.flags = (CodeFlags)(flags | ((major >= 0) ? CodeFlags::HasMajorCommandNumber : 0) | ((minor >= 0) ? CodeFlags::HasMinorCommandNumber : 0));
		h.numParameters = (uint8_t)params.size();
		h.letter = letter;
		h.majorCode = (major >= 0) ? major : 0;
		h.minorCode = (minor >= 0) ? minor : -1;
		h.filePosition = 4711;
		h.lineNumber = 42;
		Append(b, h);
		for (const Param& p : params)
		{
			CodeParameter cp;
			memset(&cp, 0, sizeof(cp));
			cp.letter = p.letter;
			cp.type = p.type;
			cp.intValue = p.intValue;
			Append(b, cp);
		}
		for (const Param& p : params)
		{
			b.insert(b.end(), p.data.begin(), p.data.end());
			while ((b.size() - ConfigBytes) % 4 != 0) { b.push_back(0); }
		}
		return b;
	}
}

static std::vector<Bytes> FuzzSeeds()
{
	std::vector<Bytes> seeds;
	const uint8_t configs[] = { 0xFF, 0xBF, 0x2E, 0x01 };
	for (uint8_t config : configs)
	{
		for (uint8_t getter = 0; getter < NumGetters; getter += (config == 0xFF) ? 1 : 4)
		{
			seeds.push_back(Code(config, getter, 4, 'G', 1, -1, 0, { FloatParam('X', 10.0f), IntParam('Y', 20), FloatParam('F', 3000.0f) }));
			seeds.push_back(Code(config, getter, 4, 'M', 98, -1, 0, { StringParam('P', "macro.g"), IntParam('S', 5), StringParam('A', "text") }));
			seeds.push_back(Code(config, getter, 4, 'M', 587, -1, 0, { StringParam('S', "ssid"), StringParam('P', "password"), StringParam('I', "192.168.1.10") }));
			seeds.push_back(Code(config, getter, 4, 'M', 540, -1, 0, { StringParam('P', "BE:EF:DE:AD:FE:ED") }));
			seeds.push_back(Code(config, getter, 4, 'M', 584, -1, 0, { ArrayParam<uint32_t>('X', DataType::DriverIdArray, { 0x10002, 0x10003 }), DriverParam('Y', 0, 1) }));
			seeds.push_back(Code(config, getter, 4, 'M', 92, -1, 0, { ArrayParam<float>('E', DataType::FloatArray, { 400.0f, 420.0f }),
																	ArrayParam<int32_t>('I', DataType::IntArray, { -1, 2, 3 }),
																	ArrayParam<uint32_t>('U', DataType::UIntArray, { 7, 8 }) }));
			seeds.push_back(Code(config, getter, 4, 'G', 1, -1, 0, { StringParam('X', "{move.axes[0].max}", DataType::Expression), StringParam('Y', "\"abc\"", DataType::Expression),
																	StringParam('Z', "h", DataType::Expression), StringParam('F', "f", DataType::Expression) }));
			seeds.push_back(Code(config, getter, 4, 'M', 117, -1, 0, { StringParam('@', "Hello world") }));
			seeds.push_back(Code(config, getter, 4, 'Q', 0, -1, 0, { StringParam('@', "a comment") }));
			seeds.push_back(Code(config, getter, 4, 'T', 1, -1, 0, { NullParam('P'), UIntParam('R', 3) }));
			seeds.push_back(Code(config, getter, 4, 'G', 10, -1, CodeFlags::HasFilePosition | CodeFlags::EnforceAbsolutePosition | CodeFlags::HasExplicitLineNumber,
								{ IntParam('P', 0), FloatParam('X', 1.0f), FloatParam('a', 2.0f) }));
			seeds.push_back(Code(config, getter, 4, 'M', 569, 1, 0, { DriverParam('P', 1, 2), StringParam('S', "1", DataType::Expression) }));
			seeds.push_back(Code(config, getter, 4, 'M', 999, -1, 0, { }));
		}
	}
	for (const Bytes& b : seeds)
	{
		// The seeds are codes as DSF writes them, so the layout model must take each of them
		if (!CodeFitsModel(b.data() + ConfigBytes, b.size() - ConfigBytes))
		{
			fprintf(stderr, "ERROR: FUZZ: a seed of %u bytes does not fit the layout model\nSUMMARY: FUZZ: seed-does-not-fit\n", (unsigned int)b.size());
			abort();
		}
	}
	return seeds;
}

// The files whose first frame in a sanitizer report is its key (fuzz_driver.inc)
static const char *const FuzzKeyFiles[] = { "BinaryParser.cpp", "StringHandle.cpp", "Heap.cpp", "GCodeException.cpp" };

// For an ASan report: where the bad address is relative to the code buffer
static void FuzzDescribeAddress(uintptr_t a) noexcept
{
	if (gb == nullptr || gb->buffer == nullptr) { return; }
	const intptr_t offset = (intptr_t)(a - reinterpret_cast<uintptr_t>(gb->buffer));
	if (offset >= -65536 && offset < (intptr_t)MaxGCodeBinaryLength + 0x40000)
	{
		fprintf(stderr, "FUZZ: bad address is codeBuffer%+" PRIdPTR " (the code buffer is %u bytes, the code %u)\n", offset,
				(unsigned int)MaxGCodeBinaryLength, (unsigned int)gb->binaryParser.DataLength());
	}
}

#ifdef FUZZ_LIBFUZZER
static void FuzzDeathCallback()
{
	if (__asan_report_present()) { FuzzDescribeAddress(reinterpret_cast<uintptr_t>(__asan_get_report_address())); }
}
extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
	__sanitizer_set_death_callback(FuzzDeathCallback);
	return 0;
}
#else
# include "../sbc-packet-fuzz/fuzz_driver.inc"
#endif
