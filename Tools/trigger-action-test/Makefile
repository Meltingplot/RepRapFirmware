# Host test of what M581 and M581.1 triggers do when they fire: the K parameter (upstream), and the action line, the lock and the fail-safe
# of the fork. It compiles the real header src/GCodes/TriggerItem.h and the real text of the TriggerItem functions, of GCodes::CheckTriggers
# (src/GCodes/GCodes.cpp) and of GCodes::ConfigureTrigger and GCodes::CheckTrigger (src/GCodes/GCodes3.cpp), extracted at build time.
# trigger_action_test.cpp gives them stand-ins that record what they call. Run with make in WSL (g++ 13).
#
#   make              build build/trigger-action-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-trigger-action-test
#   make check        build both and run them
#
# TRIGGERITEM_H, TRIGGERITEM_CPP, GCODES_CPP and GCODES3_CPP select the files under test, for example copies from before the fix.

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries
CANLIB ?= $(RRF)/../CANlib

TRIGGERITEM_H ?= $(SRC)/GCodes/TriggerItem.h
TRIGGERITEM_CPP ?= $(SRC)/GCodes/TriggerItem.cpp
GCODES_CPP ?= $(SRC)/GCodes/GCodes.cpp
GCODES3_CPP ?= $(SRC)/GCodes/GCodes3.cpp

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Istubs -I$(RRFLIBS)/src -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

# All functions of TriggerItem.cpp but EvaluateExpression, which the test replaces. GetConfiguredAction and IsLocked came with the fix: each
# is extracted when the header under test declares it, so that the text from before the fix builds as well.
FIX_FUNCTIONS := GetConfiguredAction IsLocked
TRIGGERITEM_FUNCTIONS := TriggerItem::TriggerItem TriggerItem::Init TriggerItem::IsUnused TriggerItem::GetAction TriggerItem::IsLevelActive \
	TriggerItem::Check TriggerItem::Configure TriggerItem::CheckLevel TriggerItem::GetActionLine TriggerItem::AppendInputNames \
	TriggerItem::IsAllowedInAction TriggerItem::IsAllowedAction \
	$(foreach n,$(FIX_FUNCTIONS),$(if $(shell grep -lw $(n) $(TRIGGERITEM_H)),TriggerItem::$(n)))

GEN := gen/TriggerItem.h gen/TriggerItem.inc gen/GCodes.inc gen/GCodes3.inc gen/StringParser.inc gen/Sizes.inc

.PHONY: all asan check clean FORCE
all: build/trigger-action-test
asan: build/asan-trigger-action-test

# FORCE on the files under test, because the variables may name other files than the last build

# The header itself, with a #line directive, so that the test includes the given one under its usual name
gen/TriggerItem.h: $(TRIGGERITEM_H) Makefile FORCE
	@mkdir -p gen
	{ printf '#line 1 "%s"\n' "$(abspath $<)"; tr -d '\r' < $<; } > $@.tmp && mv $@.tmp $@

gen/TriggerItem.inc: $(TRIGGERITEM_CPP) extract.awk Makefile FORCE
	$(call EXTRACT,$(TRIGGERITEM_FUNCTIONS))

gen/GCodes.inc: $(GCODES_CPP) extract.awk Makefile FORCE
	$(call EXTRACT,GCodes::CheckTriggers)

gen/GCodes3.inc: $(GCODES3_CPP) extract.awk Makefile FORCE
	$(call EXTRACT,GCodes::ConfigureTrigger GCodes::CheckTrigger)

# TriggerItem::IsAllowedAction calls the static CallsFileFunction; the name gives both overloads
gen/StringParser.inc: $(SRC)/GCodes/GCodeBuffer/StringParser.cpp extract.awk Makefile
	$(call EXTRACT,StringParser::CallsFileFunction)

# MaxTriggers as the MB6HC defines it and MaxGpInPorts of Duet 3. Fails unless both are found
gen/Sizes.inc: $(SRC)/Config/Pins_Duet3_MB6HC.h $(CANLIB)/src/Duet3Common.h Makefile
	@mkdir -p gen
	{ grep -hE '^constexpr unsigned int MaxTriggers = ' $(SRC)/Config/Pins_Duet3_MB6HC.h; grep -hE '^constexpr size_t MaxGpInPorts = ' $(CANLIB)/src/Duet3Common.h; } \
		| sed -e 's|//.*||' | tr -d '\r' > $@.tmp
	test "$$(wc -l < $@.tmp)" -eq 2 && mv $@.tmp $@

build/trigger-action-test: trigger_action_test.cpp $(GEN) $(wildcard stubs/*.h stubs/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $< -o $@

build/asan-trigger-action-test: trigger_action_test.cpp $(GEN) $(wildcard stubs/*.h stubs/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $< -o $@

check: all asan
	build/trigger-action-test
	build/asan-trigger-action-test

clean:
	rm -rf build gen
