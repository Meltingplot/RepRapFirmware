# Host test of the driver numbers of the set-driver-states and stop-drivers CAN requests in expansion mode: the real text of
# Move::EutHandleSetDriverStates and Move::StopDriversFromRemote (src/Movement/Move2.cpp), extracted at build time, with the sizes of the
# Duet 3 Mini 5+ (src/Config/Pins_Duet3Mini.h). Run with make in WSL (g++ 13).
#
#   make              build build/remote-driver-number-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-remote-driver-number-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries
EXTRACT := ../macro-error-test/extract.awk

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter -I$(RRFLIBS)/src -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

GEN := gen/Move2.inc gen/Sizes.inc

.PHONY: all asan check clean
all: build/remote-driver-number-test
asan: build/asan-remote-driver-number-test

gen/Move2.inc: $(SRC)/Movement/Move2.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="Move::EutHandleSetDriverStates Move::StopDriversFromRemote" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

# The two sizes, as the Mini 5+ defines them. Fails unless both are found
gen/Sizes.inc: $(SRC)/Config/Pins_Duet3Mini.h Makefile
	@mkdir -p gen
	grep -hE '^constexpr size_t (NumDirectDrivers|MaxAxesPlusExtruders) = ' $< | sed -e 's|//.*||' | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 2 && mv $@.tmp $@

build/remote-driver-number-test: remote_driver_number_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $< -o $@

build/asan-remote-driver-number-test: remote_driver_number_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $< -o $@

check: all asan
	build/remote-driver-number-test
	build/asan-remote-driver-number-test

clean:
	rm -rf build gen
