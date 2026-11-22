# Host test of the first binary code of a G-code buffer in SBC mode: the real text of SbcInterface::FillBuffer, extracted from
# src/SBC/SbcInterface.cpp at build time, with the scheduler suspension of TaskCriticalSectionLocker, the G-code buffer and
# Tasks::AllocPermanent played in sbc_code_buffer_alloc_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-code-buffer-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sbc-code-buffer-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/sbc-code-buffer-alloc-test
asan: build/asan-sbc-code-buffer-alloc-test

gen/FillBuffer.inc: $(SRC)/SBC/SbcInterface.cpp extract-fill-buffer.awk
	@mkdir -p gen
	awk -f extract-fill-buffer.awk $(abspath $(SRC)/SBC/SbcInterface.cpp) > $@.tmp && mv $@.tmp $@

build/sbc-code-buffer-alloc-test: sbc_code_buffer_alloc_test.cpp gen/FillBuffer.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen sbc_code_buffer_alloc_test.cpp -o $@

build/asan-sbc-code-buffer-alloc-test: sbc_code_buffer_alloc_test.cpp gen/FillBuffer.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen sbc_code_buffer_alloc_test.cpp -o $@

check: all asan
	build/sbc-code-buffer-alloc-test
	build/asan-sbc-code-buffer-alloc-test

clean:
	rm -rf build gen
