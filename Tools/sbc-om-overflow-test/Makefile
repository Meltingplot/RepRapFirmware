# Host test of a GetObjectModel request from the SBC whose response does not fit into the output buffers: the real text of its case in
# SbcInterface::ExchangeData (src/SBC/SbcInterface.cpp), of RepRap::GetModelResponse and StartJsonResponse (src/Platform/RepRap.cpp), of
# ObjectModel::ReportHeapArrayAsJson (src/ObjectModel/ObjectModel.cpp) and of the OutputBuffer functions they use
# (src/Platform/OutputMemory.cpp), extracted at build time, with the real RRFLibraries string and printf code. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-om-overflow-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-sbc-om-overflow-test
#   make check        build both and run them
#
# Run it against the source without the fix to see the cases that fail there:
#   make clean check RRF=<checkout of the previous commit> RRFLIBS=<RRFLibraries> WITHOUT_FIX=1

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf without noexcept, glibc declares them with it, and glibc's
# declarations have to come first
# -Wno-format: the message of SbcInterface prints a size_t with %u (32-bit target)
BASEFLAGS := -std=gnu++20 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-format -include cstdio
INC := -Igen -I$(RRFLIBS)/src
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

EXTRACT := ../macro-error-test/extract.awk
OUTPUT_FUNCTIONS := OutputBuffer::Length OutputBuffer::Clear OutputBuffer::UpdateWhenQueued OutputBuffer::vcatf OutputBuffer::catf \
	OutputBuffer::copy OutputBuffer::cat OutputBuffer::Init OutputBuffer::Allocate OutputBuffer::Release OutputBuffer::ReleaseAll
LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,StringRef.cpp SafeVsnprintf.cpp Strnlen.cpp StringFunctions.cpp SimpleMath.cpp)
SOURCES := sbc_om_overflow_test.cpp $(LIBSOURCES)

GEN := gen/SbcConstants.inc gen/GetObjectModelCase.inc gen/GetModelResponse.inc gen/ReportHeapArrayAsJson.inc gen/OutputMemory.inc

.PHONY: all asan check clean
all: build/sbc-om-overflow-test
asan: build/asan-sbc-om-overflow-test

# The buffer threshold of the fix and its static_assert, two lines at file scope, which extract.awk does not handle. Fails if they are missing,
# except with WITHOUT_FIX=1: the source without the fix has neither, and its case does not use them
gen/SbcConstants.inc: $(SRC)/SBC/SbcInterface.cpp Makefile
	@mkdir -p gen
	$(if $(filter 1,$(WITHOUT_FIX)),echo "// no MinimumBuffersForSbcJson in this source" > $@ && exit 0;) awk -v f="$(abspath $<)" '{ sub(/\r$$/, "") } /^(constexpr size_t MinimumBuffersForSbcJson|static_assert\(MinimumBuffersForSbcJson)/ { printf "#line %d \"%s\"\n", NR, f; print; ++n } END { exit (n == 2) ? 0 : 1 }' $< > $@.tmp && mv $@.tmp $@

gen/GetObjectModelCase.inc: $(SRC)/SBC/SbcInterface.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="case:GetObjectModel" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

# The definitions of static members start with a /*static*/ comment, which extract.awk does not take for a function head. sed removes the
# comment without changing the number of any line, so the #line directives stay right
gen/GetModelResponse.inc: $(SRC)/Platform/RepRap.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	sed 's#^/\*static \?\*/ ##' $< | awk -v f="$(abspath $<)" -v names="RepRap::GetModelResponse RepRap::StartJsonResponse" -f $(EXTRACT) > $@.tmp && mv $@.tmp $@

gen/ReportHeapArrayAsJson.inc: $(SRC)/ObjectModel/ObjectModel.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="ObjectModel::ReportHeapArrayAsJson" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/OutputMemory.inc: $(SRC)/Platform/OutputMemory.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	sed 's#^/\*static \?\*/ ##' $< | awk -v f="$(abspath $<)" -v names="$(OUTPUT_FUNCTIONS)" -f $(EXTRACT) > $@.tmp && mv $@.tmp $@

build/sbc-om-overflow-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(INC) $(SOURCES) -o $@

build/asan-sbc-om-overflow-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(INC) $(SOURCES) -o $@

check: all asan
	build/sbc-om-overflow-test
	build/asan-sbc-om-overflow-test

clean:
	rm -rf build gen
