# Host test of the integer conversions of RRFLibraries SafeVsnprintf (src/General/SafeVsnprintf.cpp), compiled unchanged, with
# UndefinedBehaviorSanitizer: printing INT_MIN with %d and LLONG_MIN with %lld. Run with make in WSL (g++ 13).
#
#   make              build build/print-int-min-test
#   make ubsan        the same with UBSan, which stops at the first undefined behaviour, build/ubsan-print-int-min-test
#   make check        build both and run them

RRF ?= ../..
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf without noexcept, glibc with it, and glibc's declarations have to
# come first
BASEFLAGS := -std=gnu++20 -fno-rtti -fexceptions -Wall -include cstdio
INC := -I$(RRFLIBS)/src
UBSANFLAGS := -fsanitize=undefined -fno-sanitize-recover=all

LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,SafeVsnprintf.cpp Strnlen.cpp)
HEADERS := $(RRFLIBS)/src/General/SafeVsnprintf.h Makefile

.PHONY: all ubsan check clean
all: build/print-int-min-test
ubsan: build/ubsan-print-int-min-test

define variant
build/$(1)/lib-%.o: $(RRFLIBS)/src/General/%.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/print_int_min_test.o: print_int_min_test.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(3): build/$(1)/print_int_min_test.o $(patsubst $(RRFLIBS)/src/General/%.cpp,build/$(1)/lib-%.o,$(LIBSOURCES))
	$$(CXX) $(2) $$^ -o $$@
endef

$(eval $(call variant,plain,,print-int-min-test))
$(eval $(call variant,ubsan,$(UBSANFLAGS),ubsan-print-int-min-test))

check: all ubsan
	build/print-int-min-test
	build/ubsan-print-int-min-test

clean:
	rm -rf build
