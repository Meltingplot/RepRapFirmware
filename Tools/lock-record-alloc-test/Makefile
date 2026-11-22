# Host test of the lock records of ReadWriteLock: a record that ReadWriteLock allocates with the scheduler suspended runs RRF's malloc
# without the malloc mutex, while a preempted task may be inside malloc (see lock_record_alloc_test.cpp). Run with make in WSL (g++ 13).
# Everything taken from the firmware is the real text, compiled from the trees below or extracted into gen/ at build time:
#
#   RRF ($(RRF)):                src/libc/nano-mallocr.c (whole file), src/Platform/Tasks.cpp ("// Mutexes" to ReleaseMallocMutex)
#   RRFLibraries ($(RRFLIB)):    src/RTOSIface/RTOSIface.h, RTOSIface.cpp (vAssertCalled to the Mutex members, and the ReadWriteLock
#                                members), General/FreelistManager.h
#   FreeRTOS ($(FREERTOS)):      src/tasks.c vTaskSuspendAll, xTaskGetSchedulerState
#
# Binaries build/[asan-]lock-record-alloc-test-<malloc>:
#   <malloc>  orig     nano-mallocr.c unchanged (the sbrk scenario's preemption point is in the _sbrk stand-in of arena.cpp)
#             hook326  nano-mallocr.c with one call inserted before line 326 (the split scenario's preemption point)
#
#   make              build the plain binaries
#   make asan         the same with AddressSanitizer and UBSan
#   make check        build both and run every scenario; each one must find no corruption and no ::operator new with the
#                     scheduler suspended
#   make check RRFLIB=<path>    the same against another RRFLibraries tree, e.g. one with a fix

RRF ?= ../..
RRFLIB ?= $(RRF)/../RRFLibraries
FREERTOS ?= $(RRF)/../FreeRTOS

CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -O1 -g
CFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra -Wno-ignored-qualifiers -Wno-unused-parameter -DRTOS -pthread
CBASE := -std=gnu11 -Wall -Wno-unused-variable -Dnoexcept= -include malloc_names.h -I$(RRF)/src/libc/eCvInclude
INC := -Ishim -I. -I$(RRFLIB)/src
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

EXTRACT = @mkdir -p gen && awk -v f="$(abspath $(2))" $(3) -f extract.awk $(2) > $@.tmp && mv $@.tmp $@

# The RRFLibraries path is part of the generated text, so a change of RRFLIB rebuilds everything that depends on it
GEN := gen/RTOSIface-mutex.inc gen/RTOSIface-rwlock.inc gen/Tasks-malloc-mutex.inc gen/vTaskSuspendAll.inc gen/xTaskGetSchedulerState.inc \
	gen/nano-mallocr-hook326.c
HDRS := $(wildcard shim/*.h) arena.h malloc_names.h $(RRFLIB)/src/RTOSIface/RTOSIface.h $(RRFLIB)/src/General/FreelistManager.h Makefile

PLAIN := build/lock-record-alloc-test-orig build/lock-record-alloc-test-hook326
ASAN := build/asan-lock-record-alloc-test-orig build/asan-lock-record-alloc-test-hook326

.PHONY: all asan check clean FORCE
all: $(PLAIN)
asan: $(ASAN)

# Rebuild when RRFLIB names another tree than at the last build
gen/rrflib.txt: FORCE
	@mkdir -p gen
	@echo '$(abspath $(RRFLIB))' | cmp -s - $@ || echo '$(abspath $(RRFLIB))' > $@

gen/RTOSIface-mutex.inc: $(RRFLIB)/src/RTOSIface/RTOSIface.cpp extract.awk gen/rrflib.txt Makefile
	$(call EXTRACT,,$<,-v start='^extern "C" [[][[]noreturn[]][]] void vAssertCalled' -v end='^Mutex [*]null Mutex::mutexList = nullptr;$$')

gen/RTOSIface-rwlock.inc: $(RRFLIB)/src/RTOSIface/RTOSIface.cpp extract.awk gen/rrflib.txt Makefile
	$(call EXTRACT,,$<,-v start='^// Class ReadWriteLock members' -v end='^// End' -v endexcl=1)

gen/Tasks-malloc-mutex.inc: $(RRF)/src/Platform/Tasks.cpp extract.awk Makefile
	$(call EXTRACT,,$<,-v start='^// Mutexes$$' -v endafter='ReleaseMallocMutex' -v end='^}$$')

gen/vTaskSuspendAll.inc: $(FREERTOS)/src/tasks.c extract.awk Makefile
	$(call EXTRACT,,$<,-v start='^void vTaskSuspendAll[(] void [)]$$' -v end='^}$$')

gen/xTaskGetSchedulerState.inc: $(FREERTOS)/src/tasks.c extract.awk Makefile
	$(call EXTRACT,,$<,-v start='^    BaseType_t xTaskGetSchedulerState[(] void [)]$$' -v end='^    }$$')

gen/nano-mallocr-hook326.c: $(RRF)/src/libc/nano-mallocr.c extract.awk Makefile
	$(call EXTRACT,,$<,-v hook='^                    r->size = alloc_size;$$' -v text='TestPreemptionPointSplit();')

# Objects. $(1) = plain or asan, $(2) = extra flags
define objects
build/$(1)/host_rtos.o: host_rtos.cpp $(GEN) $(HDRS)
	@mkdir -p build/$(1)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(2) -Ishim -c $$< -o $$@
build/$(1)/malloc_mutex.o: malloc_mutex.cpp $(GEN) $(HDRS)
	@mkdir -p build/$(1)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(2) $(INC) -c $$< -o $$@
build/$(1)/rwlock.o: rwlock.cpp $(GEN) $(HDRS)
	@mkdir -p build/$(1)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(2) $(INC) -c $$< -o $$@
build/$(1)/test.o: lock_record_alloc_test.cpp $(GEN) $(HDRS)
	@mkdir -p build/$(1)
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(2) $(INC) -DTEST_RRFLIB='"$(abspath $(RRFLIB))"' -c $$< -o $$@
endef
$(eval $(call objects,plain,))
$(eval $(call objects,asan,$(ASANFLAGS)))

# nano-mallocr.c and the arena are never instrumented: they touch the chunk headers that the ASan build keeps poisoned
build/nano-orig.o: $(RRF)/src/libc/nano-mallocr.c malloc_names.h Makefile
	@mkdir -p build
	$(CC) $(CBASE) $(CFLAGS) -c $< -o $@
build/nano-hook326.o: gen/nano-mallocr-hook326.c malloc_names.h Makefile
	@mkdir -p build
	$(CC) $(CBASE) $(CFLAGS) -c $< -o $@
build/arena.o: arena.cpp arena.h Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -c $< -o $@

# Binaries. $(1) = malloc variant
define binary
build/lock-record-alloc-test-$(1): build/plain/host_rtos.o build/plain/malloc_mutex.o build/plain/rwlock.o build/plain/test.o build/nano-$(1).o build/arena.o
	$(CXX) -pthread $$^ -o $$@
build/asan-lock-record-alloc-test-$(1): build/asan/host_rtos.o build/asan/malloc_mutex.o build/asan/rwlock.o build/asan/test.o build/nano-$(1).o build/arena.o
	$(CXX) -pthread $(ASANFLAGS) $$^ -o $$@
endef
$(foreach m,orig hook326,$(eval $(call binary,$(m))))

# Runs every scenario of both builds, prints one line per run and fails if any run fails. Run a binary by hand for its full log.
RUNS := "orig sbrk race" "orig sbrk freelist" "orig sbrk outside" "hook326 split race" "hook326 split freelist" "hook326 split outside" \
	"orig calls"

check: all asan
	@fail=0; for p in build/ build/asan-; do \
		for r in $(RUNS); do \
			set -- $$r; bin=$${p}lock-record-alloc-test-$$1; shift; \
			out=$$(ASAN_OPTIONS=detect_leaks=0 $$bin "$$@" 2>&1); rc=$$?; \
			res=$$(echo "$$out" | grep -m1 -E '^RESULT|ERROR: AddressSanitizer'); \
			if [ $$rc -eq 0 ]; then echo "ok      $$bin $$*: $$res"; else echo "FAILED  $$bin $$* (exit $$rc): $$res"; fail=1; fi; \
		done; \
	done; exit $$fail

clean:
	rm -rf build gen
