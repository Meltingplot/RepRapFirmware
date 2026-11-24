// Stack model of the expression fuzz target: the stack of the board task that evaluates the expression, played on the host.
//
// The real ExpressionParser::CheckStack (ExpressionParser.cpp:2661) and ObjectExplorationContext::CheckStack (ObjectModel.cpp:637)
// compare the stack pointer with the stack limit of the task and throw "Expression nesting too deep" when less than the called
// function's stack usage plus StackUsage::Throw + StackUsage::Margin (1050 + 300 bytes, GCodeException.h:16-17) is left. The host stack
// pointer says nothing about the board, because host frames have other sizes (and AddressSanitizer makes them larger). So the RRF code
// and RRFLibraries code of this target is compiled with -finstrument-functions, and the hooks below keep a virtual board stack: on
// entry to a function they add the frame that the same function has in the MB6HC firmware (board-stack-usage.tsv: the largest CFA
// offset of its DWARF call frame information, the "static" figure of -fstack-usage; see board-frames.py), on exit they take it off
// again. GetStackPointer() returns the virtual stack pointer, TaskBase::GetCurrentTaskStackBase() and the end of the task control block
// the virtual stack limit, so the real CheckStack functions decide with board figures.
//
// What the model reports:
// - "FUZZ: board-stack-overflow": a function entry takes the virtual stack below the limit. CheckStack did not prevent that, so this
//   is a finding (unless the frame figures are wrong, see README.md)
// - "FUZZ: no-stack-to-throw": a C++ exception is thrown with less than StackUsage::Throw left. On the board the unwinder would then
//   write below the stack (GCodeException.h:16: "how much stack we need to throw an exception")
// - SoftwareReset(stackOverflow) is reported by expression_fuzz.cpp: CheckStack found too little stack to throw, the board resets
//
// Not counted: functions that are not compiled with -finstrument-functions (libc on the host, newlib on the board: strtod, mktime,
// strptime helpers, memcpy, ...), functions whose host name does not match a board name (they count 0, e.g. those that the board
// build inlined), and the frames of the C++ unwinder itself (that is what StackUsage::Throw is for).

#include <cxxabi.h>
#include <elf.h>
#include <link.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <fstream>
#if defined(__SANITIZE_ADDRESS__)
# define FUZZ_HAVE_SANITIZER 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define FUZZ_HAVE_SANITIZER 1
# endif
#endif
#ifdef FUZZ_HAVE_SANITIZER
# include <sanitizer/common_interface_defs.h>
#endif

#define NOINSTR __attribute__((no_instrument_function))

namespace FuzzStack
{
	struct BoardFrame { const char *name; unsigned int bytes; };
	#include "board-stack-usage.inc"			// generated from board-stack-usage.tsv by the Makefile

	constexpr size_t TcbSize = 128;				// sizeof(TaskBase) of shim/ExprShim.h
	constexpr uint32_t ThrowBytes = 1050;		// StackUsage::Throw, GCodeException.h:16

	size_t budget = 0;							// bytes of board stack below the virtual stack pointer at the start of the evaluation
	size_t depth = 0;							// bytes of board stack in use by the instrumented functions
	size_t maxDepth = 0;
	bool reportEnabled = true;
	alignas(16) char area[TcbSize + 65536];		// the control block, then the stack (only addresses are used)

	std::unordered_map<uintptr_t, unsigned int> *frameByAddress = nullptr;
	std::unordered_map<uintptr_t, const char *> *nameByAddress = nullptr;

	// Keep in step with normalise() in board-frames.py
	NOINSTR static std::string StripBalanced(const std::string& s, char open, char close)
	{
		std::string out;
		int d = 0;
		for (char c : s)
		{
			if (c == open) { ++d; }
			else if (c == close && d > 0) { --d; }
			else if (d == 0) { out += c; }
		}
		return out;
	}

	NOINSTR static std::string Normalise(std::string name)
	{
		const size_t with = name.find(" [with ");
		if (with != std::string::npos) { name.resize(with); }
		// {lambda(...)#N} -> {lambda}
		for (size_t p; (p = name.find("{lambda(")) != std::string::npos; )
		{
			const size_t e = name.find('}', p);
			if (e == std::string::npos) { break; }
			name.replace(p, e - p + 1, "{lambda@}");
		}
		for (size_t p; (p = name.find("{lambda@}")) != std::string::npos; ) { name.replace(p, 9, "{lambda}"); }
		for (size_t p; (p = name.find("operator()")) != std::string::npos; ) { name.replace(p, 10, "operator@call"); }
		name = StripBalanced(name, '(', ')');
		name = StripBalanced(name, '<', '>');
		while (!name.empty() && name.back() == ' ') { name.pop_back(); }
		while (!name.empty() && name.front() == ' ') { name.erase(0, 1); }
		if (name.size() > 6 && name.compare(name.size() - 6, 6, " const") == 0) { name.resize(name.size() - 6); }
		const size_t sp = name.rfind(' ');
		if (sp != std::string::npos) { name = name.substr(sp + 1); }
		const std::string call = "::operator@call";
		if (name.size() > call.size() && name.compare(name.size() - call.size(), call.size(), call) == 0) { name.resize(name.size() - call.size()); }
		return name;
	}

	NOINSTR static int FindBase(struct dl_phdr_info *info, size_t, void *data)
	{
		*static_cast<uintptr_t *>(data) = info->dlpi_addr;
		return 1;								// the first object is the executable
	}

	// Read the function symbols of the executable (.symtab) and give each one the board frame of its normalised name
	NOINSTR static void Load()
	{
		frameByAddress = new std::unordered_map<uintptr_t, unsigned int>();
		nameByAddress = new std::unordered_map<uintptr_t, const char *>();
		std::unordered_map<std::string, unsigned int> board;
		for (const BoardFrame& b : boardFrames) { board[b.name] = b.bytes; }

		std::ifstream f("/proc/self/exe", std::ios::binary);
		const std::string elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (elf.size() < sizeof(Elf64_Ehdr)) { return; }
		const Elf64_Ehdr *eh = reinterpret_cast<const Elf64_Ehdr *>(elf.data());
		const Elf64_Shdr *sh = reinterpret_cast<const Elf64_Shdr *>(elf.data() + eh->e_shoff);
		uintptr_t base = 0;
		dl_iterate_phdr(FindBase, &base);
		for (unsigned int i = 0; i < eh->e_shnum; ++i)
		{
			if (sh[i].sh_type != SHT_SYMTAB) { continue; }
			const Elf64_Sym *syms = reinterpret_cast<const Elf64_Sym *>(elf.data() + sh[i].sh_offset);
			const char *strtab = elf.data() + sh[sh[i].sh_link].sh_offset;
			const size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
			for (size_t k = 0; k < n; ++k)
			{
				if (ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC || syms[k].st_value == 0) { continue; }
				const char *mangled = strtab + syms[k].st_name;
				int status = 0;
				char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
				const std::string key = Normalise((status == 0 && demangled != nullptr) ? demangled : mangled);
				const auto it = board.find(key);
				const uintptr_t addr = base + syms[k].st_value;
				(*nameByAddress)[addr] = strdup((status == 0 && demangled != nullptr) ? demangled : mangled);
				if (it != board.end()) { (*frameByAddress)[addr] = it->second; }
				free(demangled);
			}
		}
	}

	NOINSTR static unsigned int FrameOf(void *fn)
	{
		if (frameByAddress == nullptr) { Load(); }
		const auto it = frameByAddress->find(reinterpret_cast<uintptr_t>(fn));
		return (it == frameByAddress->end()) ? 0 : it->second;
	}

	NOINSTR static const char *NameOf(void *fn)
	{
		if (nameByAddress == nullptr) { Load(); }
		const auto it = nameByAddress->find(reinterpret_cast<uintptr_t>(fn));
		return (it == nameByAddress->end()) ? "?" : it->second;
	}

	[[noreturn]] NOINSTR static void Report(const char *what, void *fn, unsigned int frame)
	{
		fprintf(stderr, "ERROR: FUZZ: %s: entering %s (board frame %u bytes) with %zu bytes of the %zu byte board stack in use\n",
				what, NameOf(fn), frame, depth, budget);
#ifdef FUZZ_HAVE_SANITIZER
		__sanitizer_print_stack_trace();
#endif
		fprintf(stderr, "SUMMARY: FUZZ: %s\n", what);
		abort();
	}

	// Start of an evaluation: 'bytes' of board stack are available below the current virtual stack pointer
	NOINSTR void Begin(size_t bytes) noexcept
	{
		if (frameByAddress == nullptr) { Load(); }
		budget = std::min(bytes, sizeof(area) - TcbSize);
		depth = 0;
		maxDepth = 0;
	}

	// The virtual stack pointer and limit. The stack grows down from area + TcbSize + budget towards area + TcbSize
	NOINSTR const char *StackPointer() noexcept { return area + TcbSize + budget - std::min(depth, budget); }
	NOINSTR const char *StackLimit() noexcept { return area + TcbSize; }
	NOINSTR void *TaskControlBlock() noexcept { return area; }
	NOINSTR size_t Depth() noexcept { return depth; }
	NOINSTR void SetDepth(size_t d) noexcept { depth = d; }
	NOINSTR size_t MaxDepth() noexcept { return maxDepth; }
	NOINSTR size_t NumMatched() noexcept { if (frameByAddress == nullptr) { Load(); } return frameByAddress->size(); }

	// Called before a C++ exception is thrown (the target wraps __cxa_throw)
	NOINSTR void CheckThrow() noexcept
	{
		if (budget != 0 && depth + ThrowBytes > budget)
		{
			Report("no-stack-to-throw", __builtin_return_address(0), 0);
		}
	}
}

extern "C" NOINSTR void __cyg_profile_func_enter(void *fn, void *) noexcept
{
	const unsigned int frame = FuzzStack::FrameOf(fn);
	FuzzStack::depth += frame;
	if (FuzzStack::depth > FuzzStack::maxDepth) { FuzzStack::maxDepth = FuzzStack::depth; }
	if (FuzzStack::budget != 0 && FuzzStack::depth > FuzzStack::budget)
	{
		FuzzStack::Report("board-stack-overflow", fn, frame);
	}
}

extern "C" NOINSTR void __cyg_profile_func_exit(void *fn, void *) noexcept
{
	const unsigned int frame = FuzzStack::FrameOf(fn);
	FuzzStack::depth = (FuzzStack::depth >= frame) ? FuzzStack::depth - frame : 0;
}
