# Fuzz target for the binary G-codes that DSF sends to RRF in SBC mode: the real GCodes/GCodeBuffer/BinaryParser.cpp, Platform/Heap.cpp
# and Platform/StringHandle.cpp and the RRFLibraries string and number code, compiled unchanged, and the real text of
# GCodeException::GetMessage, ThrowGCodeException, DriverId and GCodeBufferState, extracted at build time. Run with make in WSL
# (g++ 13; clang 18 for the libFuzzer build).
#
#   make              build/binary-code-fuzz (g++, plain) and build/asan-binary-code-fuzz (g++, AddressSanitizer + UBSan), both with the
#                     deterministic driver of ../sbc-packet-fuzz/fuzz_driver.inc
#   make libfuzzer    build/libfuzzer-binary-code-fuzz (clang, libFuzzer + AddressSanitizer + UBSan)
#   make check        run the seeds, and a short deterministic run in which every report must be one of findings/known.txt
#   make findings     check that every input in findings/ still reports its finding
#   make coverage     line coverage of BinaryParser and StringHandle after a deterministic run (gcov)

RRF ?= ../..
SRC := $(abspath $(RRF))/src
RRFLIBS ?= $(RRF)/../RRFLibraries
CANLIB ?= $(RRF)/../CANlib
PACKET := ../sbc-packet-fuzz

CXX ?= g++
CLANGXX ?= clang++
CXXFLAGS ?= -O1 -g
# -fpermissive: Heap.cpp casts a pointer to uint32_t (32-bit target). -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf
# without noexcept, glibc with it, and glibc's declarations have to come first
BASEFLAGS := -std=gnu++20 -fno-rtti -fexceptions -fpermissive -Wall -Wno-unused-variable -Wno-unused-function -Wno-class-memaccess \
	-Wno-address-of-packed-member -include cstdio
# shim/ comes first, so that the "GCodeBuffer.h" and "ExpressionParser.h" includes of the copy of BinaryParser.cpp in gen/ find the
# stand-ins; the real BinaryParser.h is found in the last directory
INC := -Ishim -Igen -I. -I$(SRC) -I$(RRFLIBS)/src -I$(CANLIB)/src -I$(SRC)/GCodes/GCodeBuffer
# The sanitizers stop at the first report, so that each finding comes with its input. -fno-sanitize=alignment: the string heap rounds
# every allocation to 4 bytes, the alignment of everything on the target; a code parameter is 4-byte aligned like the code buffer
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer
LIBFUZZERFLAGS := -fsanitize=fuzzer,address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer -DFUZZ_LIBFUZZER \
	-Wno-unknown-warning-option

LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,StringRef.cpp SafeVsnprintf.cpp Strnlen.cpp StringFunctions.cpp SafeStrtod.cpp \
	NumericConverter.cpp IPAddress.cpp NamedEnum.cpp)
HEADERS := $(wildcard shim/*.h shim/*/*.h) $(SRC)/GCodes/GCodeBuffer/BinaryParser.h $(SRC)/SBC/SbcMessageFormats.h \
	$(SRC)/GCodes/GCodeException.h $(SRC)/Platform/Heap.h $(SRC)/Platform/StringHandle.h $(PACKET)/fuzz_driver.inc gen/RrfTypes.inc Makefile
GEN := gen/BinaryParser.cpp gen/GCodeException.inc gen/RrfTypes.inc

.PHONY: all asan libfuzzer check findings coverage clean
all: build/binary-code-fuzz build/asan-binary-code-fuzz
asan: build/asan-binary-code-fuzz
libfuzzer: build/libfuzzer-binary-code-fuzz

# BinaryParser.cpp as it is, in gen/ so that its quoted includes look in shim/ before src/GCodes/GCodeBuffer
gen/BinaryParser.cpp: $(SRC)/GCodes/GCodeBuffer/BinaryParser.cpp Makefile
	@mkdir -p gen
	(echo '#line 1 "$<"'; cat $<) > $@.tmp && mv $@.tmp $@

GEXC_FUNCTIONS := GCodeException::GetMessage line:[[noreturn]]~void~ThrowGCodeException(GCodeExceptionMessage~errMsg)~THROWS \
	line:[[noreturn]]~void~ThrowGCodeException(GCodeExceptionMessage~errMsg,~uint32_t~param)
gen/GCodeException.inc: $(SRC)/GCodes/GCodeException.cpp $(PACKET)/extract.awk Makefile
	@mkdir -p gen
	awk -v f="$<" -v names="$(GEXC_FUNCTIONS)" -f $(PACKET)/extract.awk $< > $@.tmp && mv $@.tmp $@

gen/RrfTypes.inc: $(SRC)/RepRapFirmware.h $(SRC)/GCodes/GCodeBuffer/GCodeBuffer.h $(PACKET)/extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(SRC)/RepRapFirmware.h" -v names="line:struct~DriverId" -f $(PACKET)/extract.awk $(SRC)/RepRapFirmware.h > $@.tmp
	awk -v f="$(SRC)/GCodes/GCodeBuffer/GCodeBuffer.h" -v names="line:enum~class~GCodeBufferState" -f $(PACKET)/extract.awk \
		$(SRC)/GCodes/GCodeBuffer/GCodeBuffer.h >> $@.tmp && mv $@.tmp $@

# clang (libFuzzer build only) rejects two constructs that g++ accepts: the cast of a handle pointer to uint32_t in Heap.cpp (32-bit
# target, -fpermissive on the host) and a static_assert on strlen in Configuration.h. The libFuzzer build compiles copies in which the
# cast goes through uintptr_t and the static_assert is left out; nothing else differs, and the g++ builds use the files as they are
gen/clang/Heap.cpp: $(SRC)/Platform/Heap.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/((uint32_t)slotPtr & 3)/((uint32_t)(uintptr_t)slotPtr \& 3)/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '(uint32_t)(uintptr_t)slotPtr' $@)" -eq 1
gen/clang/Config/Configuration.h: $(SRC)/Config/Configuration.h Makefile
	@mkdir -p gen/clang/Config
	(echo '#line 1 "$<"'; sed -e 's/^static_assert(MaxExpectedWebDirFilenameLength + strlen/\/\/ &/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '^// static_assert(MaxExpectedWebDirFilenameLength' $@)" -eq 1

build/libfuzzer/Heap.o: gen/clang/Heap.cpp gen/clang/Config/Configuration.h $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) -Igen/clang $(INC) -I$(SRC)/Platform -c $< -o $@

# $(1) variant, $(2) compiler, $(3) flags
define variant
ifneq ($(1),libfuzzer)
build/$(1)/Heap.o: $(SRC)/Platform/Heap.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
endif
build/$(1)/StringHandle.o: $(SRC)/Platform/StringHandle.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/BinaryParser.o: gen/BinaryParser.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/lib-%.o: $(RRFLIBS)/src/General/%.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/binary_code_fuzz.o: binary_code_fuzz.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
OBJECTS_$(1) := build/$(1)/Heap.o build/$(1)/StringHandle.o build/$(1)/BinaryParser.o build/$(1)/binary_code_fuzz.o \
	$(patsubst $(RRFLIBS)/src/General/%.cpp,build/$(1)/lib-%.o,$(LIBSOURCES))
endef

$(eval $(call variant,plain,$(CXX),))
$(eval $(call variant,asan,$(CXX),$(ASANFLAGS)))
$(eval $(call variant,libfuzzer,$(CLANGXX),$(LIBFUZZERFLAGS) -Igen/clang))
$(eval $(call variant,coverage,$(CXX),--coverage -O0 -DFUZZ_COVERAGE))

build/binary-code-fuzz: $(OBJECTS_plain)
	$(CXX) $^ -o $@
build/asan-binary-code-fuzz: $(OBJECTS_asan)
	$(CXX) $(ASANFLAGS) $^ -o $@
build/libfuzzer-binary-code-fuzz: $(OBJECTS_libfuzzer)
	$(CLANGXX) $(LIBFUZZERFLAGS) $^ -o $@
build/coverage-binary-code-fuzz: $(OBJECTS_coverage)
	$(CXX) --coverage $^ -o $@

# Every run is bounded in memory: WSL has run out of memory before
RUNENV := ASAN_OPTIONS=hard_rss_limit_mb=2048:allocator_may_return_null=1:detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1

check: all
	$(RUNENV) build/binary-code-fuzz -seeds
	$(RUNENV) build/asan-binary-code-fuzz -seeds
	$(RUNENV) build/asan-binary-code-fuzz -known findings/known.txt -seed 1 -runs 20000

findings: asan
	$(RUNENV) sh $(PACKET)/run-findings.sh build/asan-binary-code-fuzz findings

coverage: build/coverage-binary-code-fuzz
	find build/coverage -name '*.gcda' -delete
	build/coverage-binary-code-fuzz -seed 1 -runs $(or $(RUNS),100000) > /dev/null
	cd build/coverage && gcov -o . BinaryParser.o StringHandle.o > /dev/null 2>&1; true
	python3 -I $(PACKET)/coverage.py build/coverage

clean:
	rm -rf build gen
