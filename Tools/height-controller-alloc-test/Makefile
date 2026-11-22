# Host test of the allocation of the height controller task by M951 (HeightController::Configure): the real text of the function,
# extracted from src/Movement/HeightControl/HeightController.cpp at build time, with the scheduler suspension of
# TaskCriticalSectionLocker, the task classes, the G-code buffer and the allocator played in height_controller_alloc_test.cpp. Run
# with make in WSL (g++ 13).
#
#   make              build build/height-controller-alloc-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-height-controller-alloc-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all asan check clean
all: build/height-controller-alloc-test
asan: build/asan-height-controller-alloc-test

gen/HeightController.inc: $(SRC)/Movement/HeightControl/HeightController.cpp extract-height-controller.awk
	@mkdir -p gen
	awk -f extract-height-controller.awk $(abspath $(SRC)/Movement/HeightControl/HeightController.cpp) > $@.tmp && mv $@.tmp $@

build/height-controller-alloc-test: height_controller_alloc_test.cpp gen/HeightController.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen height_controller_alloc_test.cpp -o $@

build/asan-height-controller-alloc-test: height_controller_alloc_test.cpp gen/HeightController.inc Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen height_controller_alloc_test.cpp -o $@

check: all asan
	build/height-controller-alloc-test
	build/asan-height-controller-alloc-test

clean:
	rm -rf build gen
