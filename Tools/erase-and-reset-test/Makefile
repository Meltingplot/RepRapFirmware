# Host test of EraseAndReset on the SAME70 (M999 P"ERASE"): the real text of EraseAndReset, Flash::Unlock and its address helpers,
# extracted from CoreN2G at build time. erase_and_reset_test.cpp runs them against a model of the flash controller and the watchdog.
# Run with make in WSL (g++ 13).
#
#   make              build build/erase-and-reset-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-erase-and-reset-test
#   make check        build both and run them

RRF ?= ../..
COREN2G ?= $(RRF)/../CoreN2G

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/Flash.inc gen/Reset.inc

.PHONY: all asan check clean
all: build/erase-and-reset-test
asan: build/asan-erase-and-reset-test

gen/Flash.inc: $(COREN2G)/src/SAM4S_4E_E70/Flash.cpp extract.awk Makefile
	$(call EXTRACT,translate_address compute_lock_range Flash::Unlock)

gen/Reset.inc: $(COREN2G)/src/SAM4S_4E_E70/Reset.cpp extract.awk Makefile
	$(call EXTRACT,EraseAndReset)

build/erase-and-reset-test: erase_and_reset_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen erase_and_reset_test.cpp -o $@

build/asan-erase-and-reset-test: erase_and_reset_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen erase_and_reset_test.cpp -o $@

check: all asan
	build/erase-and-reset-test
	build/asan-erase-and-reset-test

clean:
	rm -rf build gen
