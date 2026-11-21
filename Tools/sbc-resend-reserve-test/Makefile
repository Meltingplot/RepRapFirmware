# Host test of the room for resend requests in the SBC transmit buffer: the real SBC/DataTransfer.cpp and Storage/CRC32.cpp,
# compiled unchanged with the shims of ../sbc-harness/shim. Run with make in WSL (g++ 13).
#
#   make              build build/sbc-resend-reserve-test (MB6HC release: SUPPORTS_SBC_OVER_USB=1)
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sbc-resend-reserve-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src
SHIM := ../sbc-harness/shim

CXX ?= g++
# -fpermissive: DataTransfer.cpp and CRC32.cpp cast pointers to uint32_t (32-bit target), as in ../sbc-harness
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -fpermissive -fno-pie -Wall -Wno-unused-variable -Wno-unused-function \
	-Wno-class-memaccess -Wno-address-of-packed-member -DSUPPORTS_SBC_OVER_USB=1
LDFLAGS := -no-pie
INC := -I$(SHIM) -I$(SRC)
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

# Every object depends on the headers and on this Makefile, so that a change of the flags rebuilds it
HEADERS := $(wildcard $(SHIM)/*.h $(SHIM)/*/*.h) $(SRC)/SBC/DataTransfer.h $(SRC)/SBC/SbcMessageFormats.h Makefile

.PHONY: all asan check clean
all: build/sbc-resend-reserve-test
asan: build/asan-sbc-resend-reserve-test

define variant
build/$(1)/DataTransfer.o: $(SRC)/SBC/DataTransfer.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) -DSRC_SBC_SBCINTERFACE_H_ $$(INC) -c $$< -o $$@
build/$(1)/CRC32.o: $(SRC)/Storage/CRC32.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/resend_reserve_test.o: resend_reserve_test.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(3): build/$(1)/DataTransfer.o build/$(1)/CRC32.o build/$(1)/resend_reserve_test.o
	$$(CXX) $(2) $$(LDFLAGS) $$^ -o $$@
endef

$(eval $(call variant,plain,,sbc-resend-reserve-test))
$(eval $(call variant,asan,$(ASANFLAGS),asan-sbc-resend-reserve-test))

check: all asan
	build/sbc-resend-reserve-test
	build/asan-sbc-resend-reserve-test

clean:
	rm -rf build
