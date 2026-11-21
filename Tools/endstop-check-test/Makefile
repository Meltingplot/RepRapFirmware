# Host test of Move::OnEndstopOrZProbeStatesChanged: the real function text, extracted from src/Movement/Move.cpp at build time
# together with the real DMState (src/Movement/DriveMovement.h) and MovementFlags (src/Movement/MoveSegment.h), with the DM lists
# and Move::CheckEndstops played in endstop_check_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/endstop-check-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-endstop-check-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

GENERATED := gen/DMState.inc gen/MovementFlags.inc gen/OnEndstopOrZProbeStatesChanged.inc

.PHONY: all asan check clean
all: build/endstop-check-test
asan: build/asan-endstop-check-test

gen/DMState.inc: $(SRC)/Movement/DriveMovement.h extract-definitions.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v types="DMState" -f extract-definitions.awk $< > $@.tmp && mv $@.tmp $@

gen/MovementFlags.inc: $(SRC)/Movement/MoveSegment.h extract-definitions.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v types="MovementFlags" -f extract-definitions.awk $< > $@.tmp && mv $@.tmp $@

gen/OnEndstopOrZProbeStatesChanged.inc: $(SRC)/Movement/Move.cpp extract-definitions.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v cls="Move::" -v names="OnEndstopOrZProbeStatesChanged" -f extract-definitions.awk $< > $@.tmp && mv $@.tmp $@

build/endstop-check-test: endstop_check_test.cpp $(GENERATED) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen endstop_check_test.cpp -o $@

build/asan-endstop-check-test: endstop_check_test.cpp $(GENERATED) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen endstop_check_test.cpp -o $@

check: all asan
	build/endstop-check-test
	build/asan-endstop-check-test

clean:
	rm -rf build gen
