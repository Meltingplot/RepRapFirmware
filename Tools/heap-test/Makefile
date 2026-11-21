# Host test of RRF's string heap: the real Platform/Heap.cpp, Platform/StringHandle.cpp and Platform/ArrayHandle.cpp, compiled
# unchanged, with the other task of heap_test.cpp hooked into Heap::DeleteSlot by the linker. Run with make in WSL (g++ 13).
#
#   make              build build/heap-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-heap-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
# -fpermissive: Heap::CheckSlotGood casts a pointer to uint32_t, -Wno-format: Heap::Diagnostics prints size_t with %u (32-bit target)
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -fpermissive -Wall -Wno-format
INC := -Ishim -I$(SRC)
WRAP := -Wl,--wrap=_ZN4Heap10DeleteSlotEPNS_9IndexSlotE
# -fno-sanitize=alignment: the heap rounds every allocation to 4 bytes, the alignment of ExpressionValue on the target; on a
# 64-bit host the handles in the array elements would need 8
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer

# Every object depends on the headers and on this Makefile, so that a change of the flags rebuilds it
HEADERS := $(wildcard shim/*.h shim/*/*.h) $(SRC)/Platform/Heap.h $(SRC)/Platform/StringHandle.h $(SRC)/Platform/ArrayHandle.h \
	$(SRC)/ObjectModel/TypeCode.h Makefile

.PHONY: all asan check clean
all: build/heap-test
asan: build/asan-heap-test

define variant
build/$(1)/Heap.o: $(SRC)/Platform/Heap.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/StringHandle.o: $(SRC)/Platform/StringHandle.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/ArrayHandle.o: $(SRC)/Platform/ArrayHandle.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/heap_test.o: heap_test.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(3): build/$(1)/Heap.o build/$(1)/StringHandle.o build/$(1)/ArrayHandle.o build/$(1)/heap_test.o
	$$(CXX) $(2) $(WRAP) $$^ -o $$@
endef

$(eval $(call variant,plain,,heap-test))
$(eval $(call variant,asan,$(ASANFLAGS),asan-heap-test))

check: all asan
	build/heap-test
	build/asan-heap-test

clean:
	rm -rf build
