# Check that RepRapFirmware declares no new reseatable raw pointer, see README.md. Needs the Arm GNU toolchain and python3, e.g.
# in the firmware build container of DuetSoftwareFramework (firmware/build.sh pointer-check).
#
#   make check        build the configurations with -g, then fail for each raw pointer that allowlist.txt does not name
#   make scan         the same check on the ELF files that are already there, without building
#   make update       build, then rewrite allowlist.txt with the pointers found (keeps the comments of existing entries)
#   make selftest     check pointer-check.py against selftest/src/sample.cpp
#
# CONFIGS selects the configurations, default those of "make all" in the top-level Makefile.

RRF ?= ../..
CONFIGS ?= Duet3_MB6HC Duet3_MB6XD Duet3Mini5plus
CROSS_COMPILE ?= arm-none-eabi-
PYTHON ?= python3
ALLOWLIST := allowlist.txt
CHECK := $(PYTHON) -I pointer-check.py --readelf $(CROSS_COMPILE)readelf

# Each configuration's ELF, e.g. Duet3_MB6HC/Duet3Firmware_MB6HC.elf
ELFS = $(foreach c,$(CONFIGS),$(RRF)/$(c)/Duet3Firmware_$(patsubst Duet3%,%,$(patsubst Duet3_%,%,$(c))).elf)

.PHONY: build check scan update selftest clean

# -B rebuilds every object, so none is left from a build without -g. DEBUG_FLAGS=-g on the command line replaces the
# Makefile's DEBUG_FLAGS; DEBUG=1 would also add -Og -DDEBUG. -g changes only the debug sections, not the code
build:
	for c in $(CONFIGS); do $(MAKE) -C $(RRF) -B DEBUG_FLAGS=-g $$c || exit 1; done

check: build
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

scan:
	$(CHECK) --allowlist $(ALLOWLIST) $(ELFS)

update: build
	$(CHECK) --allowlist $(ALLOWLIST) --update $(ELFS)

build/selftest/sample.o: selftest/src/sample.cpp Makefile
	@mkdir -p build/selftest
	cd selftest && $(CROSS_COMPILE)g++ -std=c++20 -mcpu=cortex-m7 -mthumb -O2 -g -c src/sample.cpp -o $(abspath $@)

selftest: build/selftest/sample.o
	$(CHECK) --root selftest --keys $< > build/selftest/keys.txt
	diff -u selftest/expected.txt build/selftest/keys.txt
	$(CHECK) --root selftest --allowlist selftest/expected.txt $<
	! $(CHECK) --root selftest $< > build/selftest/without-allowlist.txt
	grep -q 'error: new raw pointer that can be reseated: Sample::plain (int \*)' build/selftest/without-allowlist.txt
	@echo "selftest passed"

clean:
	rm -rf build
