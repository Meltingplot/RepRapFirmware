# Host test of a queued binary code in SBC mode: the real text of QueuedCode::AssignTo, extracted from src/GCodes/GCodeQueue.cpp at
# build time, with the queued code and the G-code buffer played in queued_code_buffer_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/queued-code-buffer-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-queued-code-buffer-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/queued-code-buffer-test
asan: build/asan-queued-code-buffer-test

gen/AssignTo.inc: $(SRC)/GCodes/GCodeQueue.cpp extract-assign-to.awk
	@mkdir -p gen
	awk -f extract-assign-to.awk $(abspath $(SRC)/GCodes/GCodeQueue.cpp) > $@.tmp && mv $@.tmp $@

build/queued-code-buffer-test: queued_code_buffer_test.cpp gen/AssignTo.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen queued_code_buffer_test.cpp -o $@

build/asan-queued-code-buffer-test: queued_code_buffer_test.cpp gen/AssignTo.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen queued_code_buffer_test.cpp -o $@

check: all asan
	build/queued-code-buffer-test
	build/asan-queued-code-buffer-test

clean:
	rm -rf build gen
