// Deterministic fuzz driver for the g++ builds of the fuzz targets in Tools/sbc-packet-fuzz and Tools/binary-code-fuzz (the clang build
// links libFuzzer instead). The target defines LLVMFuzzerTestOneInput and FuzzSeeds() before it includes this file.
//
//   <binary> -seeds                     run every seed input; a sanitizer report ends the run with its exit code
//   <binary> -write-seeds <dir>         write the seed inputs as binary files, e.g. as the corpus of the libFuzzer build
//   <binary> -seed <s> -runs <n> [-out <dir>] [-known <file>] [-max <bytes>]
//                                       n inputs, each a seed with 1 to 8 random mutations; input i depends on s and i only. The inputs
//                                       run in a child process, which the driver starts again after the input that ended it. Every
//                                       distinct sanitizer report (its SUMMARY line, without directories) is listed with the first input
//                                       that caused it, and written to <dir>/<n>.hex with the report. With -known, the exit code is 1
//                                       if a report is not listed in <file>
//   <binary> <file>...                  run the given inputs: binary, or text (.hex) with the bytes in hex and # comments
//   <binary> -classify <file>...        run each given input in a child process and print the key of its sanitizer report (or "clean")
//                                       and the file name, separated by a tab; for sorting the artifacts of a libFuzzer run and for
//                                       checking that every input in findings/ still reports its finding

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef FUZZ_COVERAGE
// The coverage build runs the inputs in child processes too, which end with _exit or a signal; both skip the exit handler that writes the
// gcov counters, so the child writes them itself (gcov adds them to what the earlier children wrote)
#include <csignal>
extern "C" void __gcov_dump(void);
static void FuzzCoverageSignal(int sig) { __gcov_dump(); _exit(128 + sig); }
static void FuzzCoverageChild() noexcept
{
	for (int sig : { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL }) { signal(sig, FuzzCoverageSignal); }
}
static void FuzzChildExit(int code) noexcept { __gcov_dump(); _exit(code); }
#else
static void FuzzCoverageChild() noexcept { }
static void FuzzChildExit(int code) noexcept { _exit(code); }
#endif

namespace FuzzDriver
{
	typedef std::vector<uint8_t> Bytes;

	uint64_t SplitMix(uint64_t& state) noexcept
	{
		uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}

	const uint8_t interesting8[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x0E, 0x7F, 0x80, 0x81, 0xFE, 0xFF };
	const uint16_t interesting16[] = { 0, 1, 2, 3, 4, 7, 8, 12, 16, 20, 24, 0x7F, 0x80, 0xFF, 0x100, 0x17C, 0x180, 0x184, 0x1FF, 0x200,
		0x3FF, 0x400, 0x1000, 0x1FFC, 0x2000, 0x2001, 0x7FFF, 0x8000, 0xC000, 0xFFFE, 0xFFFF };
	const uint32_t interesting32[] = { 0, 1, 4, 0x7F, 0x80, 0xFF, 0x100, 0x180, 0x2000, 0xFFFF, 0x10000, 0x10000000, 0x3FFFFFFF, 0x40000000,
		0x7FFFFFFF, 0x80000000, 0xC0000000, 0xFFFFFFF0, 0xFFFFFFFE, 0xFFFFFFFF };

	Bytes Mutate(const std::vector<Bytes>& seeds, uint64_t seed, uint64_t index, size_t maxSize) noexcept
	{
		uint64_t state = seed * 0x100000001B3ull ^ index;
		(void)SplitMix(state);
		Bytes in = seeds[SplitMix(state) % seeds.size()];
		if (SplitMix(state) % 8 == 0)
		{
			// Append the data part of another seed, so that inputs with more than one transfer or packet come up
			const Bytes& other = seeds[SplitMix(state) % seeds.size()];
			if (other.size() > 3) { in.insert(in.end(), other.begin() + 3, other.end()); }
		}
		const unsigned int numMutations = 1 + SplitMix(state) % 8;
		for (unsigned int m = 0; m < numMutations; ++m)
		{
			if (in.empty()) { in.push_back(0); }
			const size_t pos = SplitMix(state) % in.size();
			switch (SplitMix(state) % 10)
			{
			case 0: in[pos] ^= (uint8_t)(1u << (SplitMix(state) % 8)); break;
			case 1: in[pos] = (uint8_t)SplitMix(state); break;
			case 2: in[pos] = interesting8[SplitMix(state) % sizeof(interesting8)]; break;
			case 3:
				if (pos + 2 <= in.size())
				{
					const uint16_t v = interesting16[SplitMix(state) % (sizeof(interesting16)/sizeof(interesting16[0]))];
					in[pos] = (uint8_t)v; in[pos + 1] = (uint8_t)(v >> 8);
				}
				break;
			case 4:
				if (pos + 4 <= in.size())
				{
					const uint32_t v = interesting32[SplitMix(state) % (sizeof(interesting32)/sizeof(interesting32[0]))];
					for (size_t k = 0; k < 4; ++k) { in[pos + k] = (uint8_t)(v >> (8 * k)); }
				}
				break;
			case 5:
				{
					const size_t n = 1 + SplitMix(state) % 8;
					for (size_t k = 0; k < n; ++k) { in.insert(in.begin() + pos, (uint8_t)SplitMix(state)); }
				}
				break;
			case 6:
				{
					const size_t n = std::min<size_t>(1 + SplitMix(state) % 8, in.size() - pos);
					in.erase(in.begin() + pos, in.begin() + pos + n);
				}
				break;
			case 7: in[pos] = (uint8_t)(in[pos] + (int)(SplitMix(state) % 9) - 4); break;
			case 8:
				{
					const size_t n = std::min<size_t>(1 + SplitMix(state) % 32, in.size() - pos);
					const Bytes chunk(in.begin() + pos, in.begin() + pos + n);
					const size_t to = SplitMix(state) % in.size();
					in.insert(in.begin() + to, chunk.begin(), chunk.end());
				}
				break;
			default:
				// A 16-bit length at a 4-byte aligned offset, where the length fields of the packet headers are
				if (in.size() >= 8)
				{
					const size_t p = (SplitMix(state) % (in.size() / 2)) * 2;
					if (p + 2 <= in.size())
					{
						const uint16_t v = (uint16_t)SplitMix(state);
						in[p] = (uint8_t)v; in[p + 1] = (uint8_t)(v >> 8);
					}
				}
				break;
			}
		}
		if (in.size() > maxSize) { in.resize(maxSize); }
		return in;
	}

	std::string Hex(const Bytes& b)
	{
		static const char digits[] = "0123456789abcdef";
		std::string s;
		for (uint8_t c : b) { s += digits[c >> 4]; s += digits[c & 15]; }
		return s;
	}

	bool ReadInput(const char *path, Bytes& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) { return false; }
		std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		const std::string p(path);
		if (p.size() > 4 && p.compare(p.size() - 4, 4, ".hex") == 0)
		{
			out.clear();
			std::istringstream lines(text);
			std::string line;
			int nibble = -1;
			while (std::getline(lines, line))
			{
				if (!line.empty() && line[0] == '#') { continue; }
				for (char c : line)
				{
					if (!isxdigit((unsigned char)c)) { continue; }
					const int v = isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10);
					if (nibble < 0) { nibble = v; } else { out.push_back((uint8_t)(nibble * 16 + v)); nibble = -1; }
				}
			}
		}
		else
		{
			out.assign(text.begin(), text.end());
		}
		return true;
	}

	// The key of a sanitizer report: its SUMMARY line, or for UBSan the location and text of the "runtime error" line with the numbers in the
	// text replaced by N, both without directories, e.g. "AddressSanitizer: heap-buffer-overflow DataTransfer.cpp:660 in ..."
	std::string StripDirectories(const std::string& line)
	{
		std::string out;
		std::istringstream words(line);
		std::string w;
		while (words >> w)
		{
			const size_t slash = w.rfind('/');
			if (slash != std::string::npos) { w = w.substr(slash + 1); }
			if (!out.empty()) { out += ' '; }
			out += w;
		}
		return out;
	}

	// The first frame of the report in one of the files that the target fuzzes (FuzzKeyFiles), as "File.cpp:line in function"
	std::string FirstTargetFrame(const std::string& report, size_t from)
	{
		std::istringstream lines(report.substr(from));
		std::string line;
		while (std::getline(lines, line))
		{
			const size_t hash = line.find('#');
			const size_t in = line.find(" in ");
			if (hash == std::string::npos || in == std::string::npos || line.find(" 0x") == std::string::npos) { continue; }
			if (line.find("freed by thread") != std::string::npos || line.find("allocated by thread") != std::string::npos) { break; }
			const size_t lastSpace = line.rfind(' ');
			const std::string location = line.substr(lastSpace + 1);
			const size_t slash = location.rfind('/');
			const std::string base = (slash == std::string::npos) ? location : location.substr(slash + 1);
			for (const char *f : FuzzKeyFiles)
			{
				if (base.compare(0, strlen(f) + 1, std::string(f) + ":") == 0)
				{
					std::string func = line.substr(in + 4, lastSpace - in - 4);
					const size_t paren = func.find('(');
					if (paren != std::string::npos) { func = func.substr(0, paren); }
					std::string where = base;
					const size_t secondColon = where.find(':', strlen(f) + 1);
					if (secondColon != std::string::npos) { where = where.substr(0, secondColon); }		// without the column
					return where + " in " + func;
				}
			}
		}
		return "";
	}

	// The key of a sanitizer report: the kind of error and the first frame in a fuzzed file, e.g. "AddressSanitizer: heap-buffer-overflow at
	// DataTransfer.cpp:659 in DataTransfer::ReadGetObjectModel". UBSan: the text of its "runtime error" line with the numbers replaced by N
	std::string ReportKey(const std::string& report)
	{
		const size_t ub = report.find("runtime error: ");
		const size_t s = report.find("SUMMARY: ");
		if (ub != std::string::npos && (s == std::string::npos || ub < s))
		{
			const size_t lineStart = report.rfind('\n', ub) + 1;
			const std::string location = StripDirectories(report.substr(lineStart, ub - lineStart));
			std::string text = report.substr(ub + 15, report.find('\n', ub) - ub - 15);
			std::string normalised;
			for (size_t i = 0; i < text.size(); ++i)
			{
				if (isdigit((unsigned char)text[i]) || (text[i] == '-' && i + 1 < text.size() && isdigit((unsigned char)text[i + 1])))
				{
					normalised += 'N';
					++i;
					while (i < text.size() && (isxdigit((unsigned char)text[i]) || text[i] == 'x' || text[i] == '.')) { ++i; }
					--i;
				}
				else { normalised += text[i]; }
			}
			const std::string frame = FirstTargetFrame(report, ub);
			return "UBSan: " + location + " " + normalised + (frame.empty() ? "" : " (at " + frame + ")");
		}
		if (s == std::string::npos)
		{
			return report.empty() ? "no report (signal or abort)" : "no SUMMARY line: " + report.substr(0, std::min<size_t>(report.find('\n'), 120));
		}
		std::string kind = StripDirectories(report.substr(s + 9, report.find('\n', s) - s - 9));
		const size_t space = kind.find(' ', kind.find(' ') + 1);			// "AddressSanitizer: heap-buffer-overflow"
		if (space != std::string::npos) { kind = kind.substr(0, space); }
		const size_t e = report.find("ERROR: ");
		const std::string frame = FirstTargetFrame(report, (e == std::string::npos) ? 0 : e);
		return kind + (frame.empty() ? std::string(" (no frame in a fuzzed file)") : " at " + frame);
	}

	int Run(int argc, char **argv, const std::vector<Bytes>& seeds, size_t maxSize)
	{
		uint64_t seed = 1, runs = 0;
		const char *outDir = nullptr, *knownFile = nullptr;
		std::vector<const char *> files;
		bool runSeeds = false, classify = false;
		for (int i = 1; i < argc; ++i)
		{
			const std::string a = argv[i];
			if (a == "-seeds") { runSeeds = true; }
			else if (a == "-write-seeds" && i + 1 < argc)
			{
				for (size_t k = 0; k < seeds.size(); ++k)
				{
					std::ofstream f(std::string(argv[i + 1]) + "/seed-" + std::to_string(k), std::ios::binary);
					f.write(reinterpret_cast<const char *>(seeds[k].data()), seeds[k].size());
				}
				printf("wrote %zu seeds\n", seeds.size());
				return 0;
			}
			else if (a == "-seed" && i + 1 < argc) { seed = strtoull(argv[++i], nullptr, 0); }
			else if (a == "-runs" && i + 1 < argc) { runs = strtoull(argv[++i], nullptr, 0); }
			else if (a == "-out" && i + 1 < argc) { outDir = argv[++i]; }
			else if (a == "-known" && i + 1 < argc) { knownFile = argv[++i]; }
			else if (a == "-max" && i + 1 < argc) { maxSize = strtoull(argv[++i], nullptr, 0); }
			else if (a == "-v") { verbose = true; }
			else if (a == "-classify") { classify = true; }
			else { files.push_back(argv[i]); }
		}

		if (runSeeds)
		{
			for (const Bytes& b : seeds) { LLVMFuzzerTestOneInput(b.data(), b.size()); }
			printf("%zu seeds ran without a sanitizer report\n", seeds.size());
		}
		if (classify)
		{
			for (const char *path : files)
			{
				Bytes b;
				if (!ReadInput(path, b)) { fprintf(stderr, "cannot read %s\n", path); return 2; }
				int fds[2];
				if (pipe(fds) != 0) { perror("pipe"); return 2; }
				fflush(stdout);
				const pid_t pid = fork();
				if (pid == 0)
				{
					close(fds[0]);
					dup2(fds[1], 2);
					FuzzCoverageChild();
					LLVMFuzzerTestOneInput(b.data(), b.size());
					FuzzChildExit(0);
				}
				close(fds[1]);
				std::string report;
				char buf[4096];
				ssize_t n;
				while ((n = read(fds[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
				{
					if (n > 0 && report.size() < 65536) { report.append(buf, n); }
				}
				close(fds[0]);
				int status = 0;
				waitpid(pid, &status, 0);
				const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
				printf("%s\t%s\n", clean ? "clean" : ReportKey(report).c_str(), path);
			}
			return 0;
		}
		for (const char *path : files)
		{
			Bytes b;
			if (!ReadInput(path, b)) { fprintf(stderr, "cannot read %s\n", path); return 2; }
			printf("%s: %zu bytes\n", path, b.size());
			fflush(stdout);
			LLVMFuzzerTestOneInput(b.data(), b.size());
			printf("%s: no sanitizer report\n", path);
		}
		if (runs == 0) { return 0; }

		// The child runs the inputs from 'next' on and records the index of the input it runs in shared memory
		volatile uint64_t *const current = static_cast<volatile uint64_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
		std::map<std::string, std::pair<uint64_t, unsigned int>> reports;	// key -> first input, count
		uint64_t next = 0;
		unsigned int children = 0;
		while (next < runs)
		{
			int fds[2];
			if (pipe(fds) != 0) { perror("pipe"); return 2; }
			fflush(stdout);
			const pid_t pid = fork();
			if (pid == 0)
			{
				close(fds[0]);
				dup2(fds[1], 2);
				FuzzCoverageChild();
				for (uint64_t i = next; i < runs; ++i)
				{
					*current = i;
					const Bytes in = Mutate(seeds, seed, i, maxSize);
					LLVMFuzzerTestOneInput(in.data(), in.size());
				}
				*current = runs;
				FuzzChildExit(0);
			}
			++children;
			close(fds[1]);
			std::string report;
			char buf[4096];
			ssize_t n;
			while ((n = read(fds[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
			{
				if (n > 0 && report.size() < 65536) { report.append(buf, n); }
			}
			close(fds[0]);
			int status = 0;
			waitpid(pid, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && *current >= runs)
			{
				break;
			}
			const uint64_t failed = *current;
			const std::string key = ReportKey(report);
			auto it = reports.find(key);
			if (it == reports.end())
			{
				reports[key] = std::make_pair(failed, 1u);
				const Bytes in = Mutate(seeds, seed, failed, maxSize);
				printf("NEW  input %llu: %s\n", (unsigned long long)failed, key.c_str());
				if (outDir != nullptr)
				{
					std::ofstream f(std::string(outDir) + "/seed" + std::to_string(seed) + "-" + std::to_string(failed) + ".hex");
					f << "# seed " << seed << " input " << failed << ": " << key << "\n";
					std::istringstream lines(report);
					std::string line;
					unsigned int numLines = 0;
					while (std::getline(lines, line) && numLines < 40) { f << "# " << line << "\n"; ++numLines; }
					f << Hex(in) << "\n";
				}
			}
			else
			{
				++it->second.second;
			}
			next = failed + 1;
		}

		printf("seed %llu: %llu inputs, %u child processes, %zu distinct reports\n", (unsigned long long)seed, (unsigned long long)runs, children, reports.size());
		std::set<std::string> known;
		if (knownFile != nullptr)
		{
			std::ifstream f(knownFile);
			std::string line;
			while (std::getline(f, line)) { if (!line.empty() && line[0] != '#') { known.insert(line); } }
		}
		int result = 0;
		for (const auto& r : reports)
		{
			const bool isKnown = known.count(r.first) != 0;
			printf("%s %6u x, first input %llu: %s\n", (knownFile == nullptr) ? "  " : (isKnown ? "known" : "NEW  "), r.second.second,
					(unsigned long long)r.second.first, r.first.c_str());
			if (knownFile != nullptr && !isKnown) { result = 1; }
		}
		return result;
	}
}

#ifdef FUZZ_ASAN
// Where the bad address of an ASan report is, relative to the buffers the target knows
static void FuzzDeathCallback()
{
	if (__asan_report_present())
	{
		FuzzDescribeAddress(reinterpret_cast<uintptr_t>(__asan_get_report_address()));
	}
}
#endif

int main(int argc, char **argv)
{
#ifdef FUZZ_ASAN
	__sanitizer_set_death_callback(FuzzDeathCallback);
#endif
	return FuzzDriver::Run(argc, argv, FuzzSeeds(), FuzzMaxInputSize);
}
