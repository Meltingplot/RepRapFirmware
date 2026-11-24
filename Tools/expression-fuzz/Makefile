# Fuzz target for the expression parser: the real GCodes/GCodeBuffer/ExpressionParser.cpp, ObjectModel/ObjectModel.cpp,
# ObjectModel/Variable.cpp, Platform/Heap.cpp, Platform/StringHandle.cpp, Platform/ArrayHandle.cpp, libc/strptime.cpp and the RRFLibraries
# string and number code, compiled unchanged, with a stack model of the board task (board_stack.cpp). Run with make in WSL (g++ 13;
# clang 18 for the libFuzzer build).
#
#   make              build/expression-fuzz (g++, plain) and build/asan-expression-fuzz (g++, AddressSanitizer + UBSan), both with the
#                     deterministic driver of ../sbc-packet-fuzz/fuzz_driver.inc
#   make libfuzzer    build/libfuzzer-expression-fuzz (clang, libFuzzer + AddressSanitizer + UBSan)
#   make check        run the seeds, and a short deterministic run in which every report must be one of findings/known.txt
#   make findings     check that every input in findings/ still reports its finding
#   make coverage     gcov line coverage of the real RRF files after the seeds and a deterministic run (RUNS=<n>, default 50000)
#   make stack-table  regenerate board-stack-usage.tsv from the MB6HC firmware ELF (ELF=<path>; needs llvm-dwarfdump-18, llvm-nm-18)

RRF ?= ../..
SRC := $(abspath $(RRF))/src
RRFLIBS ?= $(RRF)/../RRFLibraries
CANLIB ?= $(RRF)/../CANlib
PACKET := ../sbc-packet-fuzz
ELF ?= $(RRF)/../ReprapFirmware/Duet3_MB6HC/Duet3Firmware_MB6HC.elf

CXX ?= g++
CLANGXX ?= clang++
CXXFLAGS ?= -O1 -g
# -fpermissive: Heap.cpp casts a pointer to uint32_t (32-bit target). -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf
# without noexcept, glibc with it, and glibc's declarations have to come first. -funsigned-char: char is unsigned on the ARM target
# (AAPCS), and isDigit/isAlpha get a char
BASEFLAGS := -std=gnu++20 -fno-rtti -fexceptions -fpermissive -funsigned-char -Wall -Wno-unused-variable -Wno-unused-function \
	-Wno-class-memaccess -Wno-address-of-packed-member -Wno-format -include cstdio
# shim/ comes first, so that the "GCodeBuffer.h" include of the copy of ExpressionParser.cpp in gen/ finds the stand-in; gen/ before src/,
# so that <ObjectModel/ObjectModel.h> is the copy with the 64-bit 'whole'; the real ExpressionParser.h is found in the last directory
INC := -Ishim -Igen -I. -I$(SRC) -I$(RRFLIBS)/src -I$(CANLIB)/src -I$(SRC)/GCodes/GCodeBuffer -I$(SRC)/ObjectModel
# The RRF and RRFLibraries code is instrumented for the stack model (board_stack.cpp); the target, the stack model and the shims are not
INSTR := -finstrument-functions
# The sanitizers stop at the first report. -fno-sanitize=alignment: the string heap rounds every allocation to 4 bytes, the alignment of
# everything on the target; the 64-bit host would want 8 for the pointers in ExpressionValue. float-cast-overflow is not part of
# -fsanitize=undefined in g++
ASANFLAGS := -fsanitize=address,undefined,float-cast-overflow -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer
LIBFUZZERFLAGS := -fsanitize=fuzzer,address,undefined,float-cast-overflow -fno-sanitize=alignment -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -DFUZZ_LIBFUZZER -Wno-unknown-warning-option
LDWRAP := -Wl,--wrap=__cxa_throw

LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,StringRef.cpp SafeVsnprintf.cpp Strnlen.cpp StringFunctions.cpp SafeStrtod.cpp \
	NumericConverter.cpp IPAddress.cpp NamedEnum.cpp IP4String.cpp)
RRFSOURCES := $(SRC)/ObjectModel/Variable.cpp $(SRC)/Platform/Heap.cpp $(SRC)/Platform/StringHandle.cpp $(SRC)/Platform/ArrayHandle.cpp \
	$(SRC)/libc/strptime.cpp
GENSOURCES := gen/ExpressionParser.cpp gen/ObjectModel/ObjectModel.cpp
HEADERS := $(wildcard shim/*.h shim/*/*.h shim/*/*/*.h) $(SRC)/GCodes/GCodeBuffer/ExpressionParser.h $(SRC)/ObjectModel/Variable.h \
	$(SRC)/GCodes/GCodeException.h $(SRC)/Platform/Heap.h $(SRC)/Platform/StringHandle.h $(SRC)/Platform/ArrayHandle.h \
	gen/ObjectModel/ObjectModel.h gen/RrfTypes.inc Makefile
GEN := $(GENSOURCES) gen/ObjectModel/ObjectModel.h gen/GCodeException.inc gen/RrfTypes.inc gen/RrfFunctions.inc gen/board-stack-usage.inc

.PHONY: all asan libfuzzer check findings coverage stack-table clean
all: build/expression-fuzz build/asan-expression-fuzz
asan: build/asan-expression-fuzz
libfuzzer: build/libfuzzer-expression-fuzz

# ExpressionParser.cpp as it is, in gen/ so that its quoted include "GCodeBuffer.h" looks in shim/ before src/GCodes/GCodeBuffer
gen/ExpressionParser.cpp: $(SRC)/GCodes/GCodeBuffer/ExpressionParser.cpp Makefile
	@mkdir -p gen
	(echo '#line 1 "$<"'; cat $<) > $@.tmp && mv $@.tmp $@

# ObjectModel.h with the one change for the 64-bit host: 'whole' must be as big as the pointers in the union (ObjectModel.h:70-73).
# ObjectModel.cpp as it is, next to it, so that its quoted include "ObjectModel.h" finds the copy
gen/ObjectModel/ObjectModel.h: $(SRC)/ObjectModel/ObjectModel.h Makefile
	@mkdir -p gen/ObjectModel
	(echo '#line 1 "$<"'; sed -e 's/^\t\tuint32_t whole;/\t\tuintptr_t whole;/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c 'uintptr_t whole;' $@)" -eq 1
gen/ObjectModel/ObjectModel.cpp: $(SRC)/ObjectModel/ObjectModel.cpp Makefile
	@mkdir -p gen/ObjectModel
	(echo '#line 1 "$<"'; cat $<) > $@.tmp && mv $@.tmp $@

GEXC_FUNCTIONS := GCodeException::GetMessage line:[[noreturn]]~void~ThrowGCodeException(GCodeExceptionMessage~errMsg)~THROWS \
	line:[[noreturn]]~void~ThrowGCodeException(GCodeExceptionMessage~errMsg,~uint32_t~param)
gen/GCodeException.inc: $(SRC)/GCodes/GCodeException.cpp $(PACKET)/extract.awk Makefile
	@mkdir -p gen
	awk -v f="$<" -v names="$(GEXC_FUNCTIONS)" -f $(PACKET)/extract.awk $< > $@.tmp && mv $@.tmp $@

gen/RrfTypes.inc: $(SRC)/RepRapFirmware.h $(PACKET)/extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(SRC)/RepRapFirmware.h" -v names="line:struct~DriverId" -f $(PACKET)/extract.awk $(SRC)/RepRapFirmware.h > $@.tmp && mv $@.tmp $@

gen/RrfFunctions.inc: $(SRC)/RepRapFirmware.cpp $(PACKET)/extract.awk Makefile
	@mkdir -p gen
	awk -v f="$<" -v names="GetFloatFormatString" -f $(PACKET)/extract.awk $< > $@.tmp && mv $@.tmp $@

# The board frame table as C++ data for board_stack.cpp
gen/board-stack-usage.inc: board-stack-usage.tsv Makefile
	@mkdir -p gen
	(echo 'const BoardFrame boardFrames[] = {'; grep -v '^#' $< | awk -F '\t' '{ gsub(/\\/, "\\\\", $$1); gsub(/"/, "\\\"", $$1); printf "\t{ \"%s\", %s },\n", $$1, $$2 }'; \
		echo '};') > $@.tmp && mv $@.tmp $@

stack-table:
	llvm-dwarfdump-18 --debug-frame $(ELF) > build/debug-frame.txt
	llvm-nm-18 -C --defined-only $(ELF) > build/nm.txt
	python3 -I board-frames.py build/debug-frame.txt build/nm.txt > board-stack-usage.tsv

# clang (libFuzzer build only) rejects the cast of a handle pointer to uint32_t in Heap.cpp (32-bit target, -fpermissive on the host) and
# a static_assert on strlen in Configuration.h. The libFuzzer build compiles copies in which the cast goes through uintptr_t and the
# static_assert is left out; nothing else differs, and the g++ builds use the files as they are
gen/clang/Heap.cpp: $(SRC)/Platform/Heap.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/((uint32_t)slotPtr & 3)/((uint32_t)(uintptr_t)slotPtr \& 3)/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '(uint32_t)(uintptr_t)slotPtr' $@)" -eq 1
gen/clang/Config/Configuration.h: $(SRC)/Config/Configuration.h Makefile
	@mkdir -p gen/clang/Config
	(echo '#line 1 "$<"'; sed -e 's/^static_assert(MaxExpectedWebDirFilenameLength + strlen/\/\/ &/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '^// static_assert(MaxExpectedWebDirFilenameLength' $@)" -eq 1

# Two more for this target: clang rejects the flexible array member of a type with a destructor in ArrayHandle.cpp:19 (the copy declares
# it as the GNU zero-length array, which has the same layout) and the static_assert on strlen in ExpressionParser.cpp:189 (left out)
gen/clang/ArrayHandle.cpp: $(SRC)/Platform/ArrayHandle.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/^\tExpressionValue elements\[\];/\tExpressionValue elements[0];/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c 'ExpressionValue elements\[0\];' $@)" -eq 1
gen/clang/ExpressionParser.cpp: $(SRC)/GCodes/GCodeBuffer/ExpressionParser.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/^\tstatic_assert(ARRAY_SIZE(priorities) == strlen(operators));/\t\/\/ &/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '^	// 	static_assert(ARRAY_SIZE(priorities)' $@)" -eq 1

build/libfuzzer/Heap.o: gen/clang/Heap.cpp gen/clang/Config/Configuration.h $(GEN) $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) $(INSTR) -Igen/clang $(INC) -I$(SRC)/Platform -c $< -o $@
build/libfuzzer/ArrayHandle.o: gen/clang/ArrayHandle.cpp gen/clang/Config/Configuration.h $(GEN) $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) $(INSTR) -Igen/clang $(INC) -I$(SRC)/Platform -c $< -o $@
build/libfuzzer/ExpressionParser.o: gen/clang/ExpressionParser.cpp gen/clang/Config/Configuration.h $(GEN) $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) $(INSTR) -Igen/clang $(INC) -c $< -o $@

# $(1) variant, $(2) compiler, $(3) flags
define variant
ifneq ($(1),libfuzzer)
build/$(1)/Heap.o: $(SRC)/Platform/Heap.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
endif
build/$(1)/Variable.o: $(SRC)/ObjectModel/Variable.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
build/$(1)/StringHandle.o: $(SRC)/Platform/StringHandle.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
ifneq ($(1),libfuzzer)
build/$(1)/ArrayHandle.o: $(SRC)/Platform/ArrayHandle.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
build/$(1)/ExpressionParser.o: gen/ExpressionParser.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
endif
build/$(1)/strptime.o: $(SRC)/libc/strptime.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
build/$(1)/ObjectModel.o: gen/ObjectModel/ObjectModel.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
build/$(1)/lib-%.o: $(RRFLIBS)/src/General/%.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INSTR) $$(INC) -c $$< -o $$@
build/$(1)/expression_fuzz.o: expression_fuzz.cpp $(GEN) $(HEADERS) $(PACKET)/fuzz_driver.inc
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/board_stack.o: board_stack.cpp $(GEN) Makefile
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) -Igen -c $$< -o $$@
OBJECTS_$(1) := build/$(1)/Heap.o build/$(1)/Variable.o build/$(1)/StringHandle.o build/$(1)/ArrayHandle.o build/$(1)/strptime.o \
	build/$(1)/ExpressionParser.o build/$(1)/ObjectModel.o build/$(1)/expression_fuzz.o build/$(1)/board_stack.o \
	$(patsubst $(RRFLIBS)/src/General/%.cpp,build/$(1)/lib-%.o,$(LIBSOURCES))
endef

$(eval $(call variant,plain,$(CXX),))
$(eval $(call variant,asan,$(CXX),$(ASANFLAGS)))
$(eval $(call variant,libfuzzer,$(CLANGXX),$(LIBFUZZERFLAGS) -Igen/clang))
$(eval $(call variant,coverage,$(CXX),--coverage -O0))

build/expression-fuzz: $(OBJECTS_plain)
	$(CXX) $(LDWRAP) $^ -o $@
build/asan-expression-fuzz: $(OBJECTS_asan)
	$(CXX) $(ASANFLAGS) $(LDWRAP) $^ -o $@
build/libfuzzer-expression-fuzz: $(OBJECTS_libfuzzer)
	$(CLANGXX) $(LIBFUZZERFLAGS) $(LDWRAP) $^ -o $@
build/coverage-expression-fuzz: $(OBJECTS_coverage)
	$(CXX) --coverage $(LDWRAP) $^ -o $@

# Every run is bounded in memory: WSL has run out of memory before
RUNENV := ASAN_OPTIONS=hard_rss_limit_mb=2048:allocator_may_return_null=1:detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1

check: all
	$(RUNENV) build/expression-fuzz -seeds
	$(RUNENV) build/asan-expression-fuzz -seeds
	$(RUNENV) build/asan-expression-fuzz -known findings/known.txt -seed 1 -runs 20000

findings: asan
	$(RUNENV) sh $(PACKET)/run-findings.sh build/asan-expression-fuzz findings

coverage: build/coverage-expression-fuzz
	find build/coverage -name '*.gcda' -delete
	$(RUNENV) build/coverage-expression-fuzz -seeds > /dev/null
	$(RUNENV) build/coverage-expression-fuzz -seed 1 -runs $(or $(RUNS),50000) > /dev/null
	cd build/coverage && gcov -n -o . ExpressionParser.o ObjectModel.o Variable.o ArrayHandle.o StringHandle.o Heap.o strptime.o 2>/dev/null \
		| grep -A1 "^File '.*/src/\(GCodes/GCodeBuffer/ExpressionParser\|ObjectModel/ObjectModel\|ObjectModel/Variable\|Platform/ArrayHandle\|Platform/StringHandle\|Platform/Heap\|libc/strptime\)\.cpp'" \
		| grep -v '^--'

clean:
	rm -rf build gen
