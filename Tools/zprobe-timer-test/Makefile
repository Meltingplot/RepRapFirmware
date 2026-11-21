# Host test of the step timer of LocalZProbe against the deletion of the probe: the real text of LocalZProbe, of the pending list
# code of StepTimer and of the M672/M502 functions of EndstopsManager, extracted from src/ at build time, with the step ISR played
# in zprobe_timer_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/zprobe-timer-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-zprobe-timer-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -Wno-unused-parameter: LocalZProbe::SendProgram does not use its reply parameter
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-unused-parameter
INC := -Ishim -Igen -I$(SRC)
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

GENERATED := gen/LocalZProbeClass.inc gen/LocalZProbe.inc gen/StepTimer.inc gen/EndstopsManager.inc gen/DeleteObject.inc
DEPS := zprobe_timer_test.cpp shim/RepRapFirmware.h $(SRC)/Movement/StepTimer.h $(GENERATED) Makefile

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" $(1) -f extract-functions.awk $< > $@.tmp && mv $@.tmp $@

.PHONY: all asan check clean
all: build/zprobe-timer-test
asan: build/asan-zprobe-timer-test

gen/LocalZProbeClass.inc: $(SRC)/Endstops/LocalZProbe.h extract-functions.awk Makefile
	$(call EXTRACT,-v decl="LocalZProbe")

gen/LocalZProbe.inc: $(SRC)/Endstops/LocalZProbe.cpp extract-functions.awk Makefile
	$(call EXTRACT,-v cls="LocalZProbe::" -v names="~LocalZProbe SendProgram TimerInterrupt Interrupt")

gen/StepTimer.inc: $(SRC)/Movement/StepTimer.cpp extract-functions.awk Makefile
	$(call EXTRACT,-v cls="StepTimer::" -v names="StepTimer SetCallback ScheduleCallbackFromIsr ScheduleCallback CancelCallbackFromIsr CancelCallback Interrupt")

gen/EndstopsManager.inc: $(SRC)/Endstops/EndstopsManager.cpp extract-functions.awk Makefile
	$(call EXTRACT,-v cls="EndstopsManager::" -v names="SetZProbeDefaults ProgramZProbe")

gen/DeleteObject.inc: $(SRC)/RepRapFirmware.h extract-functions.awk Makefile
	$(call EXTRACT,-v cls="" -v names="DeleteObject")

build/zprobe-timer-test: $(DEPS)
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(INC) zprobe_timer_test.cpp -o $@

build/asan-zprobe-timer-test: $(DEPS)
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(INC) zprobe_timer_test.cpp -o $@

check: all asan
	build/zprobe-timer-test
	build/asan-zprobe-timer-test

clean:
	rm -rf build gen
