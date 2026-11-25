# Host test of the lifetime of object model objects while another task uses the result of an expression: the real text of the readers
# (the SetVariable and EvaluateExpression cases of the SBC task with SbcInterface::MakeJsonAnswer, FileInfoParser::ProcessCustomInfo)
# and of two deleting commands (Move::SetKinematics, Tool::DeleteTool), extracted from src/ at build time, and the real ReadWriteLock
# and its lockers, extracted from RRFLibraries. object_lifetime_test.cpp runs the tasks on one simulated processor. Run with make in
# WSL (g++ 13).
#
#   make              build build/object-lifetime-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-object-lifetime-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -pthread
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/RTOSIfaceClasses.inc gen/RTOSIface.inc gen/SetKinematics.inc gen/DeleteTool.inc gen/SbcCases.inc gen/MakeJsonAnswer.inc \
	gen/ProcessCustomInfo.inc

.PHONY: all asan check clean
all: build/object-lifetime-test
asan: build/asan-object-lifetime-test

gen/RTOSIfaceClasses.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.h extract.awk Makefile
	$(call EXTRACT,class:ReadWriteLock class:ReadLocker class:WriteLocker class:WriteLockedPointer)

gen/RTOSIface.inc: $(RRFLIBS)/src/RTOSIface/RTOSIface.cpp extract.awk Makefile
	$(call EXTRACT,class:LockRecord ReadWriteLock::LockForReading ReadWriteLock::ReleaseReader ReadWriteLock::LockForWriting ReadWriteLock::ReleaseWriter)

gen/SetKinematics.inc: $(SRC)/Movement/Move.cpp extract.awk Makefile
	$(call EXTRACT,Move::SetKinematics)

gen/DeleteTool.inc: $(SRC)/Tools/Tool.cpp extract.awk Makefile
	$(call EXTRACT,Tool::DeleteTool)

gen/SbcCases.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	$(call EXTRACT,case:EvaluateExpression case:SetVariable)

# The cases write their JSON answer with SbcInterface::MakeJsonAnswer since RRF #114. A source from before, e.g. the one without the fix
# (README), has none and its cases do not call it. If the cases call it and it is not there, the link fails
gen/MakeJsonAnswer.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	if grep -q 'SbcInterface::MakeJsonAnswer(' $<; then awk -v f="$(abspath $<)" -v names="SbcInterface::MakeJsonAnswer" -f extract.awk $< > $@.tmp && mv $@.tmp $@; \
	else echo "// no SbcInterface::MakeJsonAnswer in this source" > $@; fi

gen/ProcessCustomInfo.inc: $(SRC)/Storage/FileInfoParser.cpp extract.awk Makefile
	$(call EXTRACT,FileInfoParser::ProcessCustomInfo)

build/object-lifetime-test: object_lifetime_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen object_lifetime_test.cpp -o $@

build/asan-object-lifetime-test: object_lifetime_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen object_lifetime_test.cpp -o $@

check: all asan
	build/object-lifetime-test
	build/asan-object-lifetime-test

clean:
	rm -rf build gen
