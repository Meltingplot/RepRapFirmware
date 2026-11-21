# Host test of the deletion of the laser task by Move::Exit: the real text of Move::Exit, Move::WakeLaserTask and DeleteObject,
# extracted from src/Movement/Move.cpp and src/RepRapFirmware.h at build time, with the tasks that can preempt the deletion
# played in laser_task_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/laser-task-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-laser-task-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/laser-task-test
asan: build/asan-laser-task-test

gen/LaserTask.inc: $(SRC)/RepRapFirmware.h $(SRC)/Movement/Move.cpp extract-laser-task.awk
	@mkdir -p gen
	awk -f extract-laser-task.awk $(abspath $(SRC)/RepRapFirmware.h) $(abspath $(SRC)/Movement/Move.cpp) > $@.tmp && mv $@.tmp $@

build/laser-task-test: laser_task_test.cpp gen/LaserTask.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen laser_task_test.cpp -o $@

build/asan-laser-task-test: laser_task_test.cpp gen/LaserTask.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen laser_task_test.cpp -o $@

check: all asan
	build/laser-task-test
	build/asan-laser-task-test

clean:
	rm -rf build gen
