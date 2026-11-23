# Host test of the direct reads of SerialCDC on the SAME70, which M559/M560 uploads over USB use: the real text of the SerialCDC class and
# of its direct read functions, extracted from CoreN2G at build time. usb_direct_read_cache_test.cpp runs them against a model of the
# Cortex-M7 data cache and of the USB DMA. Run with make in WSL (g++ 13).
#
#   make              build build/usb-direct-read-cache-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-usb-direct-read-cache-test
#   make check        build both and run them

RRF ?= ../..
COREN2G ?= $(RRF)/../CoreN2G

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $<)" -v names="$(1)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

GEN := gen/SerialCDCClass.inc gen/SerialCDC.inc

.PHONY: all asan check clean
all: build/usb-direct-read-cache-test
asan: build/asan-usb-direct-read-cache-test

gen/SerialCDCClass.inc: $(COREN2G)/src/SerialCDC_tusb.h extract.awk Makefile
	$(call EXTRACT,class:SerialCDC)

gen/SerialCDC.inc: $(COREN2G)/src/SerialCDC_tusb.cpp extract.awk Makefile
	$(call EXTRACT,SerialCDC::SerialCDC SerialCDC::Start SerialCDC::GetMaxPacketSize SerialCDC::TakeReceiveEndpoint SerialCDC::StartDirectRead SerialCDC::IsDirectReadComplete SerialCDC::ReturnReceiveEndpoint SerialCDC::DirectReadXferComplete)

build/usb-direct-read-cache-test: usb_direct_read_cache_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen usb_direct_read_cache_test.cpp -o $@

build/asan-usb-direct-read-cache-test: usb_direct_read_cache_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen usb_direct_read_cache_test.cpp -o $@

check: all asan
	build/usb-direct-read-cache-test
	build/asan-usb-direct-read-cache-test

clean:
	rm -rf build gen
