# Fuzz target for the packets that DSF sends to RRF in SBC mode: the real SBC/DataTransfer.cpp, Storage/CRC32.cpp and
# GCodes/GCodeFileInfo.cpp and the RRFLibraries string code, compiled unchanged, and the real text of the packet loop of
# SbcInterface::ExchangeData, of the SbcInterface functions it calls and of FillBuffer, extracted from SBC/SbcInterface.cpp at build
# time. Run with make in WSL (g++ 13; clang 18 for the libFuzzer build).
#
#   make              build/sbc-packet-fuzz (g++, plain) and build/asan-sbc-packet-fuzz (g++, AddressSanitizer + UBSan), both with the
#                     deterministic driver of fuzz_driver.inc
#   make libfuzzer    build/libfuzzer-sbc-packet-fuzz (clang, libFuzzer + AddressSanitizer + UBSan)
#   make check        run the seeds, and a short deterministic run in which every report must be one of findings/known.txt
#   make findings     check that every input in findings/ still reports its finding (run-findings.sh)
#   make coverage     line coverage of the target functions after a deterministic run (gcov)

RRF ?= ../..
SRC := $(abspath $(RRF))/src
RRFLIBS ?= $(RRF)/../RRFLibraries
CANLIB ?= $(RRF)/../CANlib

CXX ?= g++
CLANGXX ?= clang++
CXXFLAGS ?= -O1 -g
# -fpermissive: DataTransfer.cpp and CRC32.cpp cast pointers to uint32_t (32-bit target). -include cstdio: SafeVsnprintf.h declares
# snprintf and vsnprintf without noexcept, glibc with it, and glibc's declarations have to come first
BASEFLAGS := -std=gnu++20 -fno-rtti -fexceptions -fpermissive -Wall -Wno-unused-variable -Wno-unused-function -Wno-class-memaccess \
	-Wno-address-of-packed-member -include cstdio
INC := -Ishim -Igen -I. -I$(SRC) -I$(RRFLIBS)/src -I$(CANLIB)/src
# The sanitizers stop at the first report, so that each finding comes with its input. The load of a bool from a packet byte other than 0
# or 1 is reported by every Read function with a bool field (see README.md), so UBSan's bool check is off unless UBSAN_BOOL=1
UBSAN_BOOL ?= 0
NOBOOL := $(if $(filter 1,$(UBSAN_BOOL)),,-fno-sanitize=bool)
# -fno-sanitize=alignment: rxPointer is always a multiple of 4 (8-byte packet headers, every ReadData padded to 4, every data header a
# multiple of 4 long), which is all that the Cortex-M7 needs; PrintStartedHeader asks for 8 because of its time_t only
ASANFLAGS := -fsanitize=address,undefined $(NOBOOL) -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer
LIBFUZZERFLAGS := -fsanitize=fuzzer,address,undefined $(NOBOOL) -fno-sanitize=alignment -fno-sanitize-recover=all -fno-omit-frame-pointer -DFUZZ_LIBFUZZER \
	-Wno-unknown-warning-option

LIBSOURCES := $(RRFLIBS)/src/General/StringRef.cpp $(RRFLIBS)/src/General/SafeVsnprintf.cpp $(RRFLIBS)/src/General/Strnlen.cpp \
	$(RRFLIBS)/src/General/StringFunctions.cpp $(RRFLIBS)/src/General/SimpleMath.cpp
# DataTransfer.cpp sees only the stand-in of SbcInterface in FuzzShim.h, as in Tools/sbc-harness
DTFLAGS := -DSRC_SBC_SBCINTERFACE_H_
HEADERS := $(wildcard shim/*.h shim/*/*.h shim/*/*/*.h) $(SRC)/SBC/DataTransfer.h $(SRC)/SBC/SbcMessageFormats.h fuzz_driver.inc Makefile

SBC_FUNCTIONS := SbcInterface::IsDroppedByEpoch SbcInterface::RecordChannelAbort SbcInterface::InvalidateBufferedCodes SbcInterface::FillBuffer \
	SbcInterface::MakeJsonAnswer
OUTPUT_FUNCTIONS := OutputBuffer::Length OutputBuffer::Clear line:size_t~OutputBuffer::copy(const~char~*_ecv_array~src,~size_t~len) \
	line:size_t~OutputBuffer::cat(const~char~*_ecv_array~src,~size_t~len)
GEN := gen/SbcConstants.inc gen/SbcPacketLoop.inc gen/SbcFunctions.inc gen/OutputBuffer.inc

.PHONY: all asan libfuzzer check findings coverage clean
all: build/sbc-packet-fuzz build/asan-sbc-packet-fuzz
asan: build/asan-sbc-packet-fuzz
libfuzzer: build/libfuzzer-sbc-packet-fuzz

# The constants of SbcInterface.cpp for the JSON answers. Fails unless all four lines are found
gen/SbcConstants.inc: $(SRC)/SBC/SbcInterface.cpp Makefile
	@mkdir -p gen
	grep -E '^(constexpr size_t MinimumBuffersForSbcJson|static_assert\(MinimumBuffersForSbcJson|constexpr size_t MaxSbcAnswerDataLength|constexpr const char \*_ecv_array ValueTooLongForSbc) ' $< | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 4 && mv $@.tmp $@

gen/SbcPacketLoop.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="for:size_t~i~=~0;~i~<~transfer.PacketsToRead()" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/SbcFunctions.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(SBC_FUNCTIONS)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/OutputBuffer.inc: $(SRC)/Platform/OutputMemory.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(OUTPUT_FUNCTIONS)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

# clang (libFuzzer build only) rejects two constructs that g++ accepts: the casts of a pointer to uint32_t in DataTransfer.cpp (four DMA
# buffer addresses) and CRC32.cpp (three alignment tests), which a 32-bit target allows and g++ accepts with -fpermissive, and a static_assert
# on strlen in Configuration.h. The libFuzzer build compiles copies in which the casts go through uintptr_t and the static_assert is left
# out; nothing else differs, and the g++ builds use the files as they are
gen/clang/DataTransfer.cpp: $(SRC)/SBC/DataTransfer.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/= (uint32_t)\(outBuffer\|inBuffer\|&\)/= (uint32_t)(uintptr_t)\1/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '(uint32_t)(uintptr_t)' $@)" -eq 4
gen/clang/Config/Configuration.h: $(SRC)/Config/Configuration.h Makefile
	@mkdir -p gen/clang/Config
	(echo '#line 1 "$<"'; sed -e 's/^static_assert(MaxExpectedWebDirFilenameLength + strlen/\/\/ &/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c '^// static_assert(MaxExpectedWebDirFilenameLength' $@)" -eq 1

gen/clang/CRC32.cpp: $(SRC)/Storage/CRC32.cpp Makefile
	@mkdir -p gen/clang
	(echo '#line 1 "$<"'; sed -e 's/reinterpret_cast<uint32_t>(s)/(uint32_t)reinterpret_cast<uintptr_t>(s)/') < $< > $@.tmp && mv $@.tmp $@
	test "$$(grep -c 'reinterpret_cast<uintptr_t>(s)' $@)" -eq 3

build/libfuzzer/CRC32.o: gen/clang/CRC32.cpp gen/clang/Config/Configuration.h $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) -Igen/clang $(INC) -I$(SRC)/Storage -c $< -o $@

build/libfuzzer/DataTransfer.o: gen/clang/DataTransfer.cpp gen/clang/Config/Configuration.h $(HEADERS)
	@mkdir -p build/libfuzzer
	$(CLANGXX) $(BASEFLAGS) $(CXXFLAGS) $(LIBFUZZERFLAGS) $(DTFLAGS) -Igen/clang $(INC) -I$(SRC)/SBC -c $< -o $@

# $(1) variant, $(2) compiler, $(3) flags
define variant
ifneq ($(1),libfuzzer)
build/$(1)/DataTransfer.o: $(SRC)/SBC/DataTransfer.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $(DTFLAGS) $$(INC) -c $$< -o $$@
build/$(1)/CRC32.o: $(SRC)/Storage/CRC32.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
endif
build/$(1)/GCodeFileInfo.o: $(SRC)/GCodes/GCodeFileInfo.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/lib-%.o: $(RRFLIBS)/src/General/%.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
build/$(1)/sbc_packet_fuzz.o: sbc_packet_fuzz.cpp $(GEN) $(HEADERS)
	@mkdir -p build/$(1)
	$(2) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
OBJECTS_$(1) := build/$(1)/DataTransfer.o build/$(1)/CRC32.o build/$(1)/GCodeFileInfo.o build/$(1)/sbc_packet_fuzz.o \
	$(patsubst $(RRFLIBS)/src/General/%.cpp,build/$(1)/lib-%.o,$(LIBSOURCES))
endef

$(eval $(call variant,plain,$(CXX),))
$(eval $(call variant,asan,$(CXX),$(ASANFLAGS)))
$(eval $(call variant,libfuzzer,$(CLANGXX),$(LIBFUZZERFLAGS) -Igen/clang))
$(eval $(call variant,coverage,$(CXX),--coverage -O0 -DFUZZ_COVERAGE))

build/sbc-packet-fuzz: $(OBJECTS_plain)
	$(CXX) $^ -o $@
build/asan-sbc-packet-fuzz: $(OBJECTS_asan)
	$(CXX) $(ASANFLAGS) $^ -o $@
build/libfuzzer-sbc-packet-fuzz: $(OBJECTS_libfuzzer)
	$(CLANGXX) $(LIBFUZZERFLAGS) $^ -o $@
build/coverage-sbc-packet-fuzz: $(OBJECTS_coverage)
	$(CXX) --coverage $^ -o $@

# Every run is bounded in memory: WSL has run out of memory before
RUNENV := ASAN_OPTIONS=hard_rss_limit_mb=2048:allocator_may_return_null=1:detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
check: all
	$(RUNENV) build/sbc-packet-fuzz -seeds
	$(RUNENV) build/asan-sbc-packet-fuzz -seeds
	$(RUNENV) build/asan-sbc-packet-fuzz -known findings/known.txt -seed 1 -runs 20000

findings: asan
	$(RUNENV) sh run-findings.sh build/asan-sbc-packet-fuzz findings

coverage: build/coverage-sbc-packet-fuzz
	find build/coverage -name '*.gcda' -delete
	build/coverage-sbc-packet-fuzz -seed 1 -runs $(or $(RUNS),100000) > /dev/null
	cd build/coverage && gcov -o . DataTransfer.o sbc_packet_fuzz.o > /dev/null 2>&1; true
	python3 -I coverage.py build/coverage

clean:
	rm -rf build gen
