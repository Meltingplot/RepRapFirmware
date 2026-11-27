# Host test of M24 during a feed hold: the real text of RegularGCodeInput::CheckForUrgentCommand (src/GCodes/GCodeInput.cpp) with
# enum class GCodeInputState (src/GCodes/GCodeInput.h) and the feed hold block of M24 in GCodes::HandleMcode (src/GCodes/GCodes2.cpp),
# extracted at build time, with the parts of Move, GCodes and RepRap they use played in feed_hold_m24_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/feed-hold-m24-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-feed-hold-m24-test
#   make check        build both and run them
#
# GCODEINPUT_H, GCODEINPUT_CPP and GCODES2_CPP select the files under test, for example copies from before the fix.

RRF ?= ../..
SRC := $(RRF)/src

GCODEINPUT_H ?= $(SRC)/GCodes/GCodeInput.h
GCODEINPUT_CPP ?= $(SRC)/GCodes/GCodeInput.cpp
GCODES2_CPP ?= $(SRC)/GCodes/GCodes2.cpp

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

GEN := gen/GCodeInputState.inc gen/CheckForUrgentCommand.inc gen/M24HoldBlock.inc
EXTRACT = @mkdir -p gen && awk -v part=$(1) -f extract.awk $(abspath $<) > $@.tmp && mv $@.tmp $@

.PHONY: all asan check clean
all: build/feed-hold-m24-test
asan: build/asan-feed-hold-m24-test

gen/GCodeInputState.inc: $(GCODEINPUT_H) extract.awk
	$(call EXTRACT,enum)

gen/CheckForUrgentCommand.inc: $(GCODEINPUT_CPP) extract.awk
	$(call EXTRACT,scanner)

gen/M24HoldBlock.inc: $(GCODES2_CPP) extract.awk
	$(call EXTRACT,m24)

build/feed-hold-m24-test: feed_hold_m24_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) feed_hold_m24_test.cpp -o $@

build/asan-feed-hold-m24-test: feed_hold_m24_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) feed_hold_m24_test.cpp -o $@

check: all asan
	build/feed-hold-m24-test
	build/asan-feed-hold-m24-test

clean:
	rm -rf build gen
