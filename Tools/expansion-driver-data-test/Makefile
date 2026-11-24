# Host test of the driver data that RRF keeps for each expansion board: the real text of ExpansionManager::StoreDriverDirection,
# StoreDriverMode, GetDriverDirection and GetDriverMode (src/CAN/ExpansionManager.cpp) and of the HasDriver(s) members of
# ExpansionBoardData (src/CAN/ExpansionManager.h), extracted at build time. Run with make in WSL (g++ 13).
#
#   make              build build/expansion-driver-data-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-expansion-driver-data-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
EXTRACT := ../macro-error-test/extract.awk

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

GEN := gen/Accessors.inc gen/BoardMembers.inc

.PHONY: all asan check clean
all: build/expansion-driver-data-test
asan: build/asan-expansion-driver-data-test

gen/Accessors.inc: $(SRC)/CAN/ExpansionManager.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="ExpansionManager::StoreDriverDirection ExpansionManager::StoreDriverMode ExpansionManager::GetDriverDirection ExpansionManager::GetDriverMode" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

# The one-line members HasDrivers and, with the fix, HasDriver. Fails unless HasDrivers is found
gen/BoardMembers.inc: $(SRC)/CAN/ExpansionManager.h Makefile
	@mkdir -p gen
	grep -hE '^[[:space:]]*bool HasDrivers?\(' $< | tr -d '\r' > $@.tmp
	grep -q 'HasDrivers()' $@.tmp && mv $@.tmp $@

build/expansion-driver-data-test: expansion_driver_data_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $< -o $@

build/asan-expansion-driver-data-test: expansion_driver_data_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $< -o $@

check: all asan
	build/expansion-driver-data-test
	build/asan-expansion-driver-data-test

clean:
	rm -rf build gen
