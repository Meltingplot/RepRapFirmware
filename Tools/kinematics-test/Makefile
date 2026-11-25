# Host test of Move::SetKinematics: the real function text, extracted from src/Movement/Move.cpp at build time, with the object
# model's read of move.kinematics played in kinematics_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/kinematics-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-kinematics-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/kinematics-test
asan: build/asan-kinematics-test

gen/SetKinematics.inc: $(SRC)/Movement/Move.cpp extract-setkinematics.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -f extract-setkinematics.awk $< > $@.tmp && mv $@.tmp $@

# SetKinematics replaces the kinematics with ReplaceObject (src/RepRapFirmware.h)
gen/ReplaceObject.inc: $(SRC)/RepRapFirmware.h ../endstops-test/extract-functions.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v cls="" -v names="ReplaceObject" -f ../endstops-test/extract-functions.awk $< > $@.tmp && mv $@.tmp $@

build/kinematics-test: kinematics_test.cpp gen/SetKinematics.inc gen/ReplaceObject.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen kinematics_test.cpp -o $@

build/asan-kinematics-test: kinematics_test.cpp gen/SetKinematics.inc gen/ReplaceObject.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen kinematics_test.cpp -o $@

check: all asan
	build/kinematics-test
	build/asan-kinematics-test

clean:
	rm -rf build gen
