# Host test of the allocation of new DDAs by M595 (DDARing::ConfigureMovementQueue): the real text of the function, extracted from
# src/Movement/DDARing.cpp at build time, with the scheduler suspension of TaskCriticalSectionLocker and Tasks::AllocPermanent
# played in movement_queue_alloc_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/movement-queue-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-movement-queue-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/movement-queue-alloc-test
asan: build/asan-movement-queue-alloc-test

gen/MovementQueue.inc: $(SRC)/Movement/DDARing.cpp extract-movement-queue.awk
	@mkdir -p gen
	awk -f extract-movement-queue.awk $(abspath $(SRC)/Movement/DDARing.cpp) > $@.tmp && mv $@.tmp $@

build/movement-queue-alloc-test: movement_queue_alloc_test.cpp gen/MovementQueue.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen movement_queue_alloc_test.cpp -o $@

build/asan-movement-queue-alloc-test: movement_queue_alloc_test.cpp gen/MovementQueue.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen movement_queue_alloc_test.cpp -o $@

check: all asan
	build/movement-queue-alloc-test
	build/asan-movement-queue-alloc-test

clean:
	rm -rf build gen
