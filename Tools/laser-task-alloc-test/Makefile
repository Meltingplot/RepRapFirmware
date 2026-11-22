# Host test of the allocation of the laser task by Move::CreateLaserTask: the real text of the function and the definition of
# Move::laserTask, extracted from src/Movement/Move.cpp at build time, with the scheduler suspension of TaskCriticalSectionLocker,
# the task classes and the allocator played in laser_task_alloc_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/laser-task-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-laser-task-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/laser-task-alloc-test
asan: build/asan-laser-task-alloc-test

gen/CreateLaserTask.inc: $(SRC)/Movement/Move.cpp extract-create-laser-task.awk
	@mkdir -p gen
	awk -f extract-create-laser-task.awk $(abspath $(SRC)/Movement/Move.cpp) > $@.tmp && mv $@.tmp $@

build/laser-task-alloc-test: laser_task_alloc_test.cpp gen/CreateLaserTask.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen laser_task_alloc_test.cpp -o $@

build/asan-laser-task-alloc-test: laser_task_alloc_test.cpp gen/CreateLaserTask.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen laser_task_alloc_test.cpp -o $@

check: all asan
	build/laser-task-alloc-test
	build/asan-laser-task-alloc-test

clean:
	rm -rf build gen
