# Host test of M954 (CanInterface::SwitchToExpansionMode): the real text of CanInterface::SwitchToExpansionMode,
# GCodes::SwitchToExpansionMode, FilamentMonitor::DeleteAll and DeleteObject, extracted from src/CAN/CanInterface.cpp,
# src/GCodes/GCodes3.cpp, src/FilamentMonitors/FilamentMonitor.cpp and src/RepRapFirmware.h at build time, with the scheduler
# suspension of TaskCriticalSectionLocker, the write lock and the filament monitors played in expansion_mode_alloc_test.cpp. Run with
# make in WSL (g++ 13).
#
#   make              build build/expansion-mode-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-expansion-mode-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
SOURCES := $(SRC)/RepRapFirmware.h $(SRC)/CAN/CanInterface.cpp $(SRC)/GCodes/GCodes3.cpp $(SRC)/FilamentMonitors/FilamentMonitor.cpp

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/expansion-mode-alloc-test
asan: build/asan-expansion-mode-alloc-test

gen/ExpansionMode.inc: $(SOURCES) extract-expansion-mode.awk
	@mkdir -p gen
	awk -f extract-expansion-mode.awk $(abspath $(SOURCES)) > $@.tmp && mv $@.tmp $@

build/expansion-mode-alloc-test: expansion_mode_alloc_test.cpp gen/ExpansionMode.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen expansion_mode_alloc_test.cpp -o $@

build/asan-expansion-mode-alloc-test: expansion_mode_alloc_test.cpp gen/ExpansionMode.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen expansion_mode_alloc_test.cpp -o $@

check: all asan
	build/expansion-mode-alloc-test
	build/asan-expansion-mode-alloc-test

clean:
	rm -rf build gen
