# Host test of the code length limits of a G-code buffer in SBC mode: the real text of StringParser::Put, AddToChecksum and
# StoreAndAddToChecksum, of BinaryParser::Put and of GCodeBuffer::AllocateBinaryBuffer, and the real MaxGCodeStringLength and MaxGCodeBinaryLength,
# extracted from src/ at build time, with the G-code buffer and Tasks::AllocPermanent played in sbc_code_length_test.cpp. Run with
# make in WSL (g++ 13).
#
#   make              build build/sbc-code-length-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sbc-code-length-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
EXTRACT := ../macro-error-test/extract.awk

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

GEN := gen/Lengths.inc gen/StringParser.inc gen/BinaryParser.inc gen/GCodeBuffer.inc

.PHONY: all asan check clean
all: build/sbc-code-length-test
asan: build/asan-sbc-code-length-test

# The two limits, as the firmware defines them. Fails unless both are found
gen/Lengths.inc: $(SRC)/Config/Configuration.h $(SRC)/SBC/SbcMessageFormats.h Makefile
	@mkdir -p gen
	grep -hE '^constexpr size_t (MaxGCodeStringLength|MaxGCodeBinaryLength) = ' $(SRC)/Config/Configuration.h $(SRC)/SBC/SbcMessageFormats.h | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 2 && mv $@.tmp $@

gen/StringParser.inc: $(SRC)/GCodes/GCodeBuffer/StringParser.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="StringParser::AddToChecksum StringParser::StoreAndAddToChecksum StringParser::Put" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/BinaryParser.inc: $(SRC)/GCodes/GCodeBuffer/BinaryParser.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="BinaryParser::Put BinaryParser::ParametersFit" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/GCodeBuffer.inc: $(SRC)/GCodes/GCodeBuffer/GCodeBuffer.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="GCodeBuffer::AllocateBinaryBuffer" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

build/sbc-code-length-test: sbc_code_length_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen sbc_code_length_test.cpp -o $@

build/asan-sbc-code-length-test: sbc_code_length_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen sbc_code_length_test.cpp -o $@

check: all asan
	build/sbc-code-length-test
	build/asan-sbc-code-length-test

clean:
	rm -rf build gen
