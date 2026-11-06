# Host harness for RRF's SBC SPI link: the real SBC/DataTransfer.cpp and Storage/CRC32.cpp, compiled unchanged, plus the
# real SbcInterface::TaskLoop text, extracted from SBC/SbcInterface.cpp at build time. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-harness-usb1 (MB6HC release: SUPPORTS_SBC_OVER_USB=1) and build/sbc-harness-usb0
#   make asan         the same with AddressSanitizer and UBSan, in build/asan-*
#   make check        run the smoke, link capability and boot delay tests against both binaries

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
# -fpermissive: DataTransfer.cpp and CRC32.cpp cast pointers to uint32_t (32-bit target). -fno-pie/-no-pie keeps every
# DMA buffer below 4 GB, so the harness can turn the XDMAC address registers back into pointers.
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -fpermissive -fno-pie -Wall -Wno-unused-variable -Wno-unused-function \
	-Wno-class-memaccess -Wno-address-of-packed-member
LDFLAGS := -no-pie
INC := -Ishim -I$(SRC) -Igen
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

GEN := gen/TaskLoop.inc gen/StateNames.inc
VARIANTS := usb1 usb0

.PHONY: all asan check clean
all: build/sbc-harness-usb1 build/sbc-harness-usb0
asan: build/asan-sbc-harness-usb1 build/asan-sbc-harness-usb0

gen/TaskLoop.inc: $(SRC)/SBC/SbcInterface.cpp extract-taskloop.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -f extract-taskloop.awk $< > $@.tmp && mv $@.tmp $@

gen/StateNames.inc: $(SRC)/SBC/DataTransfer.h extract-states.awk
	@mkdir -p gen
	awk -f extract-states.awk $< > $@.tmp && mv $@.tmp $@

define variant
build/$(1)/%.o: $(SRC)/SBC/%.cpp shim/HarnessShim.h
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) -DSUPPORTS_SBC_OVER_USB=$(3) -DSRC_SBC_SBCINTERFACE_H_ $$(INC) -c $$< -o $$@
build/$(1)/CRC32.o: $(SRC)/Storage/CRC32.cpp shim/HarnessShim.h
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) -DSUPPORTS_SBC_OVER_USB=$(3) $$(INC) -c $$< -o $$@
build/$(1)/harness.o: harness.cpp shim/HarnessShim.h $(GEN)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) -DSUPPORTS_SBC_OVER_USB=$(3) $$(INC) -c $$< -o $$@
build/$(4): build/$(1)/DataTransfer.o build/$(1)/CRC32.o build/$(1)/harness.o
	$$(CXX) $(2) $$(LDFLAGS) $$^ -o $$@
endef

$(eval $(call variant,usb1,,1,sbc-harness-usb1))
$(eval $(call variant,usb0,,0,sbc-harness-usb0))
$(eval $(call variant,asan-usb1,$(ASANFLAGS),1,asan-sbc-harness-usb1))
$(eval $(call variant,asan-usb0,$(ASANFLAGS),0,asan-sbc-harness-usb0))

check: all
	python3 -I smoke_test.py build/sbc-harness-usb1 build/sbc-harness-usb0
	python3 -I link_test.py build/sbc-harness-usb1 build/sbc-harness-usb0
	python3 -I boot_delay_test.py build/sbc-harness-usb1 build/sbc-harness-usb0

clean:
	rm -rf build gen
