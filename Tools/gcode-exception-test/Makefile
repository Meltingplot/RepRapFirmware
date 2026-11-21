# Host test of the GCodeException that a failed heater switch-on or a failed remote endstop enable throws with its reply text:
# the real GCodes/GCodeException.cpp and the RRFLibraries string code, compiled unchanged, and the real text of the throwing
# functions, extracted from Heater.cpp, SwitchEndstop.cpp and CanInterface.cpp at build time. Run with make in WSL (g++ 13).
#
#   make              build build/gcode-exception-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-gcode-exception-test
#   make compile-check  check that a GCodeException message must be a compile-time constant (compile-check/)
#   make check        build both and run them, and run the compile check

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries
CANLIB ?= $(RRF)/../CANlib

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf without noexcept, glibc declares them with it, and glibc's
# declarations have to come first
BASEFLAGS := -std=gnu++20 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -include cstdio
INC := -Ishim -Igen -I$(SRC) -I$(RRFLIBS)/src -I$(CANLIB)/src
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

THROWERS := $(SRC)/Heating/Heater.cpp $(SRC)/Endstops/SwitchEndstop.cpp $(SRC)/CAN/CanInterface.cpp
LIBSOURCES := $(RRFLIBS)/src/General/StringRef.cpp $(RRFLIBS)/src/General/SafeVsnprintf.cpp $(RRFLIBS)/src/General/Strnlen.cpp \
	$(RRFLIBS)/src/General/StringFunctions.cpp $(RRFLIBS)/src/General/SimpleMath.cpp
SOURCES := gcode_exception_test.cpp $(SRC)/GCodes/GCodeException.cpp $(LIBSOURCES)
HEADERS := $(wildcard shim/*.h shim/*/*.h shim/*/*/*.h) $(SRC)/GCodes/GCodeException.h Makefile

.PHONY: all asan compile-check check clean
all: build/gcode-exception-test
asan: build/asan-gcode-exception-test

gen/Throwers.inc: $(THROWERS) extract-throwers.awk
	@mkdir -p gen
	awk -f extract-throwers.awk $(abspath $(THROWERS)) > $@.tmp && mv $@.tmp $@

build/gcode-exception-test: $(SOURCES) gen/Throwers.inc $(HEADERS)
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(INC) $(SOURCES) -o $@

build/asan-gcode-exception-test: $(SOURCES) gen/Throwers.inc $(HEADERS)
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(INC) $(SOURCES) -o $@

compile-check: compile-check/run.sh compile-check/accept.cpp compile-check/reject.cpp $(HEADERS)
	sh compile-check/run.sh $(CXX) $(BASEFLAGS) $(INC)

check: all asan compile-check
	build/gcode-exception-test
	build/asan-gcode-exception-test

clean:
	rm -rf build gen
