# Host test of the Z probe deleters of EndstopsManager against the active endstop list: the real function text, extracted from
# src/Endstops/EndstopsManager.cpp and src/RepRapFirmware.h at build time, with the step ISR and the CAN receiver task that walk
# the list played in endstops_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/endstops-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-endstops-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

FUNCTIONS := AddToActive EnableZProbe CheckEndstops SetZProbeDefaults HandleM558
GENERATED := gen/EndstopsManager.inc gen/DeleteObject.inc

.PHONY: all asan check clean
all: build/endstops-test
asan: build/asan-endstops-test

gen/EndstopsManager.inc: $(SRC)/Endstops/EndstopsManager.cpp extract-functions.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v cls="EndstopsManager::" -v names="$(FUNCTIONS)" -f extract-functions.awk $< > $@.tmp && mv $@.tmp $@

gen/DeleteObject.inc: $(SRC)/RepRapFirmware.h extract-functions.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v cls="" -v names="DeleteObject" -f extract-functions.awk $< > $@.tmp && mv $@.tmp $@

build/endstops-test: endstops_test.cpp $(GENERATED) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen endstops_test.cpp -o $@

build/asan-endstops-test: endstops_test.cpp $(GENERATED) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen endstops_test.cpp -o $@

check: all asan
	build/endstops-test
	build/asan-endstops-test

clean:
	rm -rf build gen
