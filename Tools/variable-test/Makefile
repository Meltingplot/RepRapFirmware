# Host test of the values that RRF's variables keep: the real ObjectModel/Variable.cpp, Platform/Heap.cpp, Platform/StringHandle.cpp
# and Platform/ArrayHandle.cpp, compiled unchanged, with the object model played in variable_test.cpp. Run with make in WSL (g++ 13).
#
#   make              build build/variable-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-variable-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
# -fpermissive: Heap::CheckSlotGood casts a pointer to uint32_t, -Wno-format: Heap::Diagnostics prints size_t with %u (32-bit target)
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -fexceptions -fpermissive -Wall -Wno-format
# shim/ comes first: its RepRapFirmware.h and ObjectModel/ObjectModel.h replace those of the heap test, whose shim provides the rest
INC := -Ishim -I../heap-test/shim -I$(SRC)
# -fno-sanitize=alignment: the heap rounds every allocation to 4 bytes, the alignment of ExpressionValue on the target; on a
# 64-bit host the pointers in the array elements would need 8
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer

# Every object depends on the headers and on this Makefile, so that a change of the flags rebuilds it
HEADERS := $(wildcard shim/*.h shim/*/*.h shim/*/*/*.h ../heap-test/shim/*.h ../heap-test/shim/*/*.h) $(SRC)/Platform/Heap.h \
	$(SRC)/Platform/StringHandle.h $(SRC)/Platform/ArrayHandle.h $(SRC)/ObjectModel/TypeCode.h $(SRC)/ObjectModel/Variable.h Makefile
SOURCES := $(SRC)/Platform/Heap.cpp $(SRC)/Platform/StringHandle.cpp $(SRC)/Platform/ArrayHandle.cpp $(SRC)/ObjectModel/Variable.cpp \
	variable_test.cpp

.PHONY: all asan check clean
all: build/variable-test
asan: build/asan-variable-test

# $(1) variant, $(2) source file, $(3) extra flags
define object
build/$(1)/$(notdir $(2:.cpp=.o)): $(2) $$(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(3) $$(INC) -c $$< -o $$@
endef

# $(1) variant, $(2) extra flags, $(3) executable
define variant
$(foreach s,$(SOURCES),$(eval $(call object,$(1),$(s),$(2))))
build/$(3): $(foreach s,$(SOURCES),build/$(1)/$(notdir $(s:.cpp=.o)))
	$$(CXX) $(2) $$^ -o $$@
endef

$(eval $(call variant,plain,,variable-test))
$(eval $(call variant,asan,$(ASANFLAGS),asan-variable-test))

check: all asan
	build/variable-test
	build/asan-variable-test

clean:
	rm -rf build
