# Check that RepRapFirmware reaches the heap from no new place inside a protected region, and that no new place lifts the mask
# of a region that masks interrupts, see README.md. Needs the Arm GNU toolchain and python3, e.g. in the firmware build
# container of DuetSoftwareFramework (firmware/build.sh shell).
#
#   make check        build the configurations with -g (as Tools/pointer-check does), then run both checks
#   make scan         the heap check on the ELF files that are already there, without building: fail for each finding that
#                     allowlist.txt does not name (the release workflow runs it after Tools/pointer-check has built them)
#   make scan-unmask  the unmask check on the same ELF files: fail for each finding that unmask-allowlist.txt does not name
#   make update       rewrite both allowlists with the findings of the ELF files that are there (keeps the comments of
#                     existing entries)
#   make selftest     check alloc-check.py against selftest/src/sample.cpp and selftest/src/no-alloc.cpp
#
# CONFIGS selects the configurations, default those of "make all" in the top-level Makefile.

RRF ?= ../..
CONFIGS ?= Duet3_MB6HC Duet3_MB6XD Duet3Mini5plus
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
ALLOWLIST := allowlist.txt
UNMASK_ALLOWLIST := unmask-allowlist.txt
CHECK := $(PYTHON) -I alloc-check.py --objdump $(CROSS_COMPILE)objdump --nm $(CROSS_COMPILE)nm --addr2line $(CROSS_COMPILE)addr2line

# Each configuration's ELF, e.g. Duet3_MB6HC/Duet3Firmware_MB6HC.elf
ELFS = $(foreach c,$(CONFIGS),$(RRF)/$(c)/Duet3Firmware_$(patsubst Duet3%,%,$(patsubst Duet3_%,%,$(c))).elf)

.PHONY: build check scan scan-unmask update selftest clean

# The same build as Tools/pointer-check: the check reads the source lines from the debug information
build:
	$(MAKE) -C ../pointer-check build RRF=$(abspath $(RRF)) CONFIGS="$(CONFIGS)"

# Both checks, also when the first one fails
check: build
	$(MAKE) -k scan scan-unmask

scan:
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

scan-unmask:
	$(CHECK) --unmask --allowlist $(UNMASK_ALLOWLIST) $(ELFS)

update:
	$(CHECK) --allowlist $(ALLOWLIST) --update $(ELFS)
	$(CHECK) --unmask --allowlist $(UNMASK_ALLOWLIST) --update $(ELFS)

# .far at 0x20000000 is out of the range of bl, so that the linker calls the functions there through veneers
SELFTEST_FLAGS := -std=gnu++20 -mcpu=cortex-m7 -mthumb -O2 -g -fno-exceptions -fno-rtti -nostdlib -nostartfiles -Wl,-e,Reset_Handler \
	-Wl,--section-start=.far=0x20000000

build/selftest/%.elf: selftest/src/%.cpp selftest/src/runtime.cpp selftest/src/runtime.h Makefile
	@mkdir -p build/selftest
	cd selftest && $(CROSS_COMPILE)g++ $(SELFTEST_FLAGS) src/$*.cpp src/runtime.cpp -o $(abspath $@)

selftest: build/selftest/sample.elf build/selftest/no-alloc.elf
	$(CHECK) --root selftest --keys $< > build/selftest/keys.txt
	diff -u selftest/expected.txt build/selftest/keys.txt
	$(CHECK) --root selftest --allowlist selftest/expected.txt $<
	! $(CHECK) --root selftest $< > build/selftest/without-allowlist.txt
	grep -q 'src/sample.cpp:32: error: heap reached inside a protected region: SuspendedNew \[suspended\] -> operator new' build/selftest/without-allowlist.txt
	grep -q 'src/sample.cpp:81: error: call through a register inside a protected region: SuspendedCallback' build/selftest/without-allowlist.txt
	$(CHECK) --root selftest --unmask --keys $< > build/selftest/unmask-keys.txt
	diff -u selftest/expected-unmask.txt build/selftest/unmask-keys.txt
	$(CHECK) --root selftest --unmask --allowlist selftest/expected-unmask.txt $<
	! $(CHECK) --root selftest --unmask $< > build/selftest/unmask-without-allowlist.txt
	grep -q 'src/sample.cpp:[0-9]*: error: interrupts unmasked inside a protected region: BoostedNotify \[basepri\] -> vPortExitCritical' build/selftest/unmask-without-allowlist.txt
	printf '# header\nGone [suspended] -> malloc  # an entry of a finding that no longer exists\n' > build/selftest/update.txt
	$(CHECK) --root selftest --allowlist build/selftest/update.txt --update build/selftest/no-alloc.elf
	printf '# header\n' | diff -u - build/selftest/update.txt
	printf '# header\nGone [basepri] -> vPortExitCritical  # an entry of a finding that no longer exists\n' > build/selftest/update.txt
	$(CHECK) --root selftest --unmask --allowlist build/selftest/update.txt --update build/selftest/no-alloc.elf
	printf '# header\n' | diff -u - build/selftest/update.txt
	@echo "selftest passed"

clean:
	rm -rf build
