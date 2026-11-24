# Host test of the timeout checks that compare a clock reading with a time stamp that another task or an ISR writes: the real text of
# RemoteHeater::GetTemperature, GetAveragePWM, GetMode, RemoteFan::Check, LocalFan::GetRPM, the remote branches of FilamentMonitor and
# PulsedFilamentMonitor::Check and IsLocalMotionDetected, extracted at build time. stale_timestamp_test.cpp runs them with a writer that stores a newer stamp right after the clock read.
# Run with make in WSL (g++ 13).
#
#   make              build build/stale-timestamp-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-stale-timestamp-test
#   make check        build both and run them

RRF ?= ../..

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/RemoteHeater.inc gen/RemoteFan.inc gen/LocalFan.inc gen/FilamentMonitor.inc gen/PulsedFilamentMonitor.inc

.PHONY: all asan check clean
all: build/stale-timestamp-test
asan: build/asan-stale-timestamp-test

gen/RemoteHeater.inc: $(RRF)/src/Heating/RemoteHeater.cpp extract.awk Makefile
	$(call EXTRACT,RemoteHeater::GetTemperature RemoteHeater::GetAveragePWM RemoteHeater::GetMode)

gen/RemoteFan.inc: $(RRF)/src/Fans/RemoteFan.cpp extract.awk Makefile
	$(call EXTRACT,RemoteFan::Check)

gen/LocalFan.inc: $(RRF)/src/Fans/LocalFan.cpp extract.awk Makefile
	$(call EXTRACT,LocalFan::GetRPM)

gen/FilamentMonitor.inc: $(RRF)/src/FilamentMonitors/FilamentMonitor.cpp extract.awk Makefile
	$(call EXTRACT,FilamentMonitor::FilamentPresenceKnown FilamentMonitor::IsFilamentPresent FilamentMonitor::IsMotionDetected)

gen/PulsedFilamentMonitor.inc: $(RRF)/src/FilamentMonitors/PulsedFilamentMonitor.cpp extract.awk Makefile
	$(call EXTRACT,PulsedFilamentMonitor::Interrupt PulsedFilamentMonitor::Check PulsedFilamentMonitor::IsLocalMotionDetected)

build/stale-timestamp-test: stale_timestamp_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen stale_timestamp_test.cpp -o $@

build/asan-stale-timestamp-test: stale_timestamp_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen stale_timestamp_test.cpp -o $@

check: all asan
	build/stale-timestamp-test
	build/asan-stale-timestamp-test

clean:
	rm -rf build gen
