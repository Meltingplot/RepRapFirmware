# Host test of the ObjectModelKeyChanged request from the SBC: the real text of its case in SbcInterface::ExchangeData
# (src/SBC/SbcInterface.cpp) and of the DataTransfer functions that read its packet (src/SBC/DataTransfer.cpp and .h), extracted at build
# time, with the real RRFLibraries string code. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-key-changed-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-sbc-key-changed-test
#   make check        build both and run them
#
# The source without the fix has no DataTransfer::ReadObjectModelKeyChanged, so leave it out to run against it:
#   make clean check RRF=<checkout of the previous commit> TRANSFER_FUNCTIONS="DataTransfer::ReadData DataTransfer::ReadDataHeader"

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf without noexcept, glibc declares them with it, and glibc's
# declarations have to come first
BASEFLAGS := -std=gnu++20 -fno-rtti -Wall -Wextra -include cstdio
INC := -Igen -I$(RRFLIBS)/src
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

EXTRACT := ../macro-error-test/extract.awk
TRANSFER_FUNCTIONS ?= DataTransfer::ReadData DataTransfer::ReadDataHeader DataTransfer::ReadObjectModelKeyChanged
LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,StringRef.cpp SafeVsnprintf.cpp Strnlen.cpp StringFunctions.cpp SimpleMath.cpp)
SOURCES := sbc_key_changed_test.cpp $(LIBSOURCES)

GEN := gen/KeyChangedCase.inc gen/DataTransferRead.inc gen/AddPadding.inc

.PHONY: all asan check clean
all: build/sbc-key-changed-test
asan: build/asan-sbc-key-changed-test

gen/KeyChangedCase.inc: $(SRC)/SBC/SbcInterface.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="case:ObjectModelKeyChanged" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/DataTransferRead.inc: $(SRC)/SBC/DataTransfer.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(TRANSFER_FUNCTIONS)" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/AddPadding.inc: $(SRC)/SBC/DataTransfer.h $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="DataTransfer::AddPadding" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

build/sbc-key-changed-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(INC) $(SOURCES) -o $@

build/asan-sbc-key-changed-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(INC) $(SOURCES) -o $@

check: all asan
	build/sbc-key-changed-test
	build/asan-sbc-key-changed-test

clean:
	rm -rf build gen
