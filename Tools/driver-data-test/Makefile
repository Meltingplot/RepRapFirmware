# Host test of the driver data accessors of ExpansionManager: the real function text, extracted from src/CAN/ExpansionManager.cpp
# at build time, with the CAN receiver task that replaces the array played in driver_data_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/driver-data-test
#   make check        build it and run it

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra

.PHONY: all check clean
all: build/driver-data-test

gen/Accessors.inc: $(SRC)/CAN/ExpansionManager.cpp extract-accessors.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -f extract-accessors.awk $< > $@.tmp && mv $@.tmp $@

build/driver-data-test: driver_data_test.cpp gen/Accessors.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen driver_data_test.cpp -o $@

check: all
	build/driver-data-test

clean:
	rm -rf build gen
