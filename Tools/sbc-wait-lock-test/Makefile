# Host test of the rule that no task waits for the SBC task while it holds a ReadWriteLock for writing (SbcInterface::WaitForFileSlot,
# GCodeBuffer::RequestMacroFile), and of the code that used to break it. It compiles the real text of the functions, extracted from src/
# and RRFLibraries at build time; sbc_wait_lock_test.cpp runs the tasks on one simulated processor. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-wait-lock-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sbc-wait-lock-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/RTOSIfaceClasses.inc gen/RTOSIface.inc gen/SbcInterface.inc gen/ProcessAnnouncement.inc gen/RemoteBoardRestarted.inc \
	gen/SysFileExists.inc gen/StringParser.inc gen/TriggerItem.inc

.PHONY: all asan check clean
all: build/sbc-wait-lock-test
asan: build/asan-sbc-wait-lock-test

gen/RTOSIfaceClasses.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.h extract.awk Makefile
	$(call EXTRACT,class:ReadWriteLock class:ReadLocker class:WriteLocker class:ReadLockedPointer)

gen/RTOSIface.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.cpp extract.awk Makefile
	$(call EXTRACT,class:LockRecord NewLockRecord ReadWriteLock::LockForReading ReadWriteLock::ReleaseReader ReadWriteLock::LockForWriting ReadWriteLock::ConditionalLockForWriting ReadWriteLock::ReleaseWriter ReadWriteLock::DowngradeWriter)

gen/SbcInterface.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	$(call EXTRACT,SbcInterface::FileExists SbcInterface::TruncateFile SbcInterface::CloseFile SbcInterface::WaitForFileSlot SbcInterface::PostFileOperation SbcInterface::DoFileOperation)

gen/ProcessAnnouncement.inc: $(SRC)/CAN/ExpansionManager.cpp extract.awk Makefile
	$(call EXTRACT,ExpansionManager::ProcessAnnouncement)

gen/RemoteBoardRestarted.inc: $(SRC)/Accelerometers/Accelerometers.cpp extract.awk Makefile
	$(call EXTRACT,Accelerometers::RemoteBoardRestarted)

gen/SysFileExists.inc: $(SRC)/Platform/Platform.cpp extract.awk Makefile
	$(call EXTRACT,Platform::SysFileExists)

gen/StringParser.inc: $(SRC)/GCodes/GCodeBuffer/StringParser.cpp extract.awk Makefile
	$(call EXTRACT,StringParser::ProcessConditionalGCode StringParser::CallsFileFunction StringParser::SendGlobalAssignmentToSbc)

gen/TriggerItem.inc: $(SRC)/GCodes/TriggerItem.cpp extract.awk Makefile
	$(call EXTRACT,TriggerItem::IsAllowedInAction TriggerItem::IsAllowedAction)

build/sbc-wait-lock-test: sbc_wait_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen sbc_wait_lock_test.cpp -o $@

build/asan-sbc-wait-lock-test: sbc_wait_lock_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen sbc_wait_lock_test.cpp -o $@

check: all asan
	build/sbc-wait-lock-test
	build/asan-sbc-wait-lock-test

clean:
	rm -rf build gen
