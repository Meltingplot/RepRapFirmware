# Host test that the linker scripts of RRF refuse a firmware image that does not fit in the flash (see README.md). It needs the ARM
# toolchain, so run it in the build image, from the folder above RepRapFirmware:
#
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" -w /work/RepRapFirmware/Tools/flash-overflow-test dsf-firmware-build make check
#
#   make check        link the test images against every script and check the results
#   SCRIPTS=...       the linker scripts to check, by default every script under src/Hardware that sets _firmware_crc

RRF ?= ../..
CROSS_COMPILE ?= arm-none-eabi-
SCRIPTS ?= $(sort $(shell grep -l '_firmware_crc' $(RRF)/src/Hardware/*/*.ld))

.PHONY: check clean
check:
	./flash_overflow_test.sh "$(CROSS_COMPILE)" build $(SCRIPTS)

clean:
	rm -rf build
