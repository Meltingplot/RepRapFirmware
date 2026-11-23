# Host test of the announcement of an expansion board that restarts during a remote accelerometer collection, in SBC mode: the real
# text of ExpansionManager::ProcessAnnouncement and Accelerometers::RemoteBoardRestarted, extracted from src/ at build time, and the real
# ReadWriteLock and its lockers, extracted from RRFLibraries. announcement_lock_test.cpp runs the CAN receiver and the SBC task on one
# simulated processor. Run with make in WSL (g++ 13).
#
#   make              build build/announcement-lock-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-announcement-lock-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/RTOSIfaceClasses.inc gen/RTOSIface.inc gen/ProcessAnnouncement.inc gen/RemoteBoardRestarted.inc

.PHONY: all asan check clean
all: build/announcement-lock-test
asan: build/asan-announcement-lock-test

gen/RTOSIfaceClasses.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.h extract.awk Makefile
	$(call EXTRACT,class:ReadWriteLock class:ReadLocker class:WriteLocker)

gen/RTOSIface.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.cpp extract.awk Makefile
	$(call EXTRACT,class:LockRecord ReadWriteLock::LockForReading ReadWriteLock::ReleaseReader ReadWriteLock::LockForWriting ReadWriteLock::ConditionalLockForWriting ReadWriteLock::ReleaseWriter)

gen/ProcessAnnouncement.inc: $(SRC)/CAN/ExpansionManager.cpp extract.awk Makefile
	$(call EXTRACT,ExpansionManager::ProcessAnnouncement)

gen/RemoteBoardRestarted.inc: $(SRC)/Accelerometers/Accelerometers.cpp extract.awk Makefile
	$(call EXTRACT,Accelerometers::RemoteBoardRestarted)

build/announcement-lock-test: announcement_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen announcement_lock_test.cpp -o $@

build/asan-announcement-lock-test: announcement_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen announcement_lock_test.cpp -o $@

check: all asan
	build/announcement-lock-test
	build/asan-announcement-lock-test

clean:
	rm -rf build gen
