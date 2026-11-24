# Check that the SAME70 firmware starts no XDMAC channel without a memory barrier on the way there, see README.md. Needs the Arm
# GNU toolchain and python3, e.g. in the firmware build container of DuetSoftwareFramework (firmware/build.sh shell). Uses the
# program model of Tools/alloc-check.
#
#   make check        build the SAME70 configurations with -g (as Tools/pointer-check does), then run the check
#   make scan         the check on the ELF files that are already there, without building: fail for each finding that
#                     allowlist.txt does not name
#   make update       rewrite allowlist.txt with the findings of the ELF files that are there (keeps the comments of existing
#                     entries)
#   make selftest     check dma-barrier-check.py against selftest/src/sample.cpp
#
# CONFIGS selects the configurations, default the SAME70 ones of "make all" in the top-level Makefile.

RRF ?= ../..
CONFIGS ?= Duet3_MB6HC Duet3_MB6XD
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
ALLOWLIST := allowlist.txt
CHECK := $(PYTHON) -I dma-barrier-check.py --objdump $(CROSS_COMPILE)objdump --nm $(CROSS_COMPILE)nm --addr2line $(CROSS_COMPILE)addr2line

# Each configuration's ELF, e.g. Duet3_MB6HC/Duet3Firmware_MB6HC.elf
ELFS = $(foreach c,$(CONFIGS),$(RRF)/$(c)/Duet3Firmware_$(patsubst Duet3%,%,$(patsubst Duet3_%,%,$(c))).elf)

.PHONY: build check scan update selftest clean

# The same build as Tools/pointer-check: the check reads the source lines from the debug information
build:
	$(MAKE) -C ../pointer-check build RRF=$(abspath $(RRF)) CONFIGS="$(CONFIGS)"

check: build
	$(MAKE) scan

scan:
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

update:
	$(CHECK) --allowlist $(ALLOWLIST) --update $(ELFS)

SELFTEST_FLAGS := -std=gnu++20 -mcpu=cortex-m7 -mthumb -O2 -g -fno-exceptions -fno-rtti -nostdlib -nostartfiles -Wl,-e,Reset_Handler

build/selftest/%.elf: selftest/src/%.cpp Makefile
	@mkdir -p build/selftest
	cd selftest && $(CROSS_COMPILE)g++ $(SELFTEST_FLAGS) src/$*.cpp -o $(abspath $@)

selftest: build/selftest/sample.elf
	$(CHECK) --root selftest --keys $< > build/selftest/keys.txt
	diff -u selftest/expected.txt build/selftest/keys.txt
	$(CHECK) --root selftest --allowlist selftest/expected.txt $<
	! $(CHECK) --root selftest $< > build/selftest/without-allowlist.txt
	grep -q 'src/sample.cpp:[0-9]*: error: XDMAC channel started without a memory barrier before it: StartChannel <- UnflushedStart <- Reset_Handler' build/selftest/without-allowlist.txt
	printf '# header\nGone <- Reset_Handler  # an entry of a finding that no longer exists\n' > build/selftest/update.txt
	$(CHECK) --root selftest --allowlist build/selftest/update.txt --update $<
	(printf '# header\n'; cat selftest/expected.txt) | diff -u - build/selftest/update.txt
	@echo "selftest passed"

clean:
	rm -rf build
