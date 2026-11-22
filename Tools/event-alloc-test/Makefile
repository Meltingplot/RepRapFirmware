# Host test of the allocation of a new event by Event::AddEventV: the real text of the Event constructor, Event::AddEvent,
# Event::AddEventV and Event::FinishedProcessing, extracted from src/Platform/Event.cpp at build time, with the scheduler suspension
# of TaskCriticalSectionLocker and the allocator played in event_alloc_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/event-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-event-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/event-alloc-test
asan: build/asan-event-alloc-test

gen/Event.inc: $(SRC)/Platform/Event.cpp extract-event.awk
	@mkdir -p gen
	awk -f extract-event.awk $(abspath $(SRC)/Platform/Event.cpp) > $@.tmp && mv $@.tmp $@

build/event-alloc-test: event_alloc_test.cpp gen/Event.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen event_alloc_test.cpp -o $@

build/asan-event-alloc-test: event_alloc_test.cpp gen/Event.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen event_alloc_test.cpp -o $@

check: all asan
	build/event-alloc-test
	build/asan-event-alloc-test

clean:
	rm -rf build gen
