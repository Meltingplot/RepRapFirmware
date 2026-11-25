# Host test of a macro that DSF could not run (SbcRequest::MacroCompleted with an error): the real text of the SBC task's case, of the main
# task's apply and of the stack functions, extracted from src/ at build time, with the main task's M122 walk preempted by the SBC task in
# macro_error_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/macro-error-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-macro-error-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

GEN := gen/MacroCompletedCase.inc gen/SbcInterface.inc gen/GCodeBuffer.inc gen/GCodeMachineState.inc gen/SpinPop.inc

.PHONY: all asan check clean
all: build/macro-error-test
asan: build/asan-macro-error-test

gen/MacroCompletedCase.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="case:MacroCompleted" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/SbcInterface.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="SbcInterface::RecordChannelAbort SbcInterface::ApplyRecordedAborts SbcInterface::InvalidateChannel SbcInterface::FillBuffer" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/GCodeBuffer.inc: $(SRC)/GCodes/GCodeBuffer/GCodeBuffer.cpp extract.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="GCodeBuffer::PopState GCodeBuffer::CurrentFileMachineState GCodeBuffer::AbortFile GCodeBuffer::Diagnostics" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/GCodeMachineState.inc: $(SRC)/GCodes/GCodeMachineState.cpp extract.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="GCodeMachineState::DoingFile GCodeMachineState::CloseFile GCodeMachineState::Pop" -f extract.awk $< > $@.tmp && mv $@.tmp $@

# The pop of a failed macro at the start of GCodes::SpinGCodeBuffer: the block "if (gb.IsFailedMacroPending())" at one tab
gen/SpinPop.inc: $(SRC)/GCodes/GCodes.cpp
	@mkdir -p gen
	awk -v f="$(abspath $<)" '{ sub(/\r$$/, "") } $$0 == "\tif (gb.IsFailedMacroPending())" { p = 1; printf "#line %d \"%s\"\n", NR, f } p { print } p && $$0 == "\t}" { found = 1; exit } END { if (!found) { print "the pop of a failed macro not found in " f > "/dev/stderr"; exit 1 } }' $< > $@.tmp && mv $@.tmp $@

build/macro-error-test: macro_error_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen macro_error_test.cpp -o $@

build/asan-macro-error-test: macro_error_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen macro_error_test.cpp -o $@

check: all asan
	build/macro-error-test
	build/asan-macro-error-test

clean:
	rm -rf build gen
