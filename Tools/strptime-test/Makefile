# Host test of RRF's SafeStrptime (src/libc/strptime.cpp), compiled unchanged with the RRFLibraries number code it uses: valid dates
# and months outside 1 to 12 or years outside the range of mktime. Run with make in WSL (g++ 13).
#
#   make              build build/strptime-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-strptime-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeStrtod.h needs size_t and the standard declarations first
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -include cstdio -I$(RRFLIBS)/src
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

SOURCES := strptime_test.cpp $(SRC)/libc/strptime.cpp $(addprefix $(RRFLIBS)/src/General/,SafeStrtod.cpp NumericConverter.cpp StringFunctions.cpp)

.PHONY: all asan check clean
all: build/strptime-test
asan: build/asan-strptime-test

build/strptime-test: $(SOURCES) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(SOURCES) -o $@

build/asan-strptime-test: $(SOURCES) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(SOURCES) -o $@

check: all asan
	build/strptime-test
	build/asan-strptime-test

clean:
	rm -rf build
