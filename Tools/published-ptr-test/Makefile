# Host test of the pointer class PublishedPtr: the real src/Platform/PublishedPtr.h and PublishedPtr.cpp, compiled unchanged, with the
# real ReadWriteLock of RRFLibraries, extracted from src/RTOSIface/RTOSIface.cpp at build time, on the host RTOS of shim/. Run with make
# in WSL (g++ 13).
#
#   make              build build/published-ptr-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-published-ptr-test
#   make nochecks     both without the assertions (PUBLISHED_PTR_TEST_NO_CHECKS): the cases that depend on them must fail
#   make check        build all four and run them

RRF ?= ../..
SRC := $(RRF)/src
RRFLIB ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -Wno-ignored-qualifiers: Mutex::GetConstHandle in RRFLibraries RTOSIface.h:139 casts to a const-qualified pointer type
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-ignored-qualifiers -DRTOS -pthread
INC := -Ishim -Igen -I$(SRC) -I$(RRFLIB)/src
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
NOCHECKS := -DPUBLISHED_PTR_TEST_NO_CHECKS

# Every object depends on the headers and on this Makefile, so that a change of the flags rebuilds it
HEADERS := $(wildcard shim/*.h) gen/ReadWriteLock.inc $(SRC)/Platform/PublishedPtr.h $(SRC)/Platform/TaskPriorities.h \
	$(RRFLIB)/src/RTOSIface/RTOSIface.h $(RRFLIB)/src/General/FreelistManager.h Makefile

.PHONY: all asan nochecks check clean
all: build/published-ptr-test
asan: build/asan-published-ptr-test
nochecks: build/nochecks-published-ptr-test build/asan-nochecks-published-ptr-test

gen/ReadWriteLock.inc: $(RRFLIB)/src/RTOSIface/RTOSIface.cpp extract-rwlock.awk
	@mkdir -p gen
	awk -v f="$(abspath $<)" -f extract-rwlock.awk $< > $@.tmp && mv $@.tmp $@

define variant
build/$(1)/PublishedPtr.o: $(SRC)/Platform/PublishedPtr.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/rwlock.o: rwlock.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(1)/host_rtos.o: host_rtos.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) -c $$< -o $$@
build/$(1)/published_ptr_test.o: published_ptr_test.cpp $(HEADERS)
	@mkdir -p build/$(1)
	$$(CXX) $$(BASEFLAGS) $$(CXXFLAGS) $(2) $$(INC) -c $$< -o $$@
build/$(3): build/$(1)/PublishedPtr.o build/$(1)/rwlock.o build/$(1)/host_rtos.o build/$(1)/published_ptr_test.o
	$$(CXX) -pthread $(2) $$^ -o $$@
endef

$(eval $(call variant,plain,,published-ptr-test))
$(eval $(call variant,asan,$(ASANFLAGS),asan-published-ptr-test))
$(eval $(call variant,nochecks,$(NOCHECKS),nochecks-published-ptr-test))
$(eval $(call variant,asan-nochecks,$(ASANFLAGS) $(NOCHECKS),asan-nochecks-published-ptr-test))

check: all asan nochecks
	build/published-ptr-test
	build/asan-published-ptr-test
	build/nochecks-published-ptr-test
	build/asan-nochecks-published-ptr-test

clean:
	rm -rf build gen
