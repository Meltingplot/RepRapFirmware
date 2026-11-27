#!/bin/bash
# Checks that each linker script given stops the link when the image does not fit in its flash (rom) region. Run by "make check".
#
#   flash_overflow_test.sh <cross prefix> <build dir> <linker script>...
#
# For each script it links an object with a chosen amount of code (.text) and initialised data (.data, which the script places with
# .relocate after _etext) three times:
#   fits     the CRC that the build appends at _firmware_crc ends at most 7 bytes before the end of rom: the link must succeed;
#   crc      the same image 8 bytes larger, so the CRC ends beyond rom: the link must fail with the assertion message;
#   data     code that fits, but .relocate's load image crosses the end of rom: the link must fail with the assertion message.
# Without the assertion the last two link without an error, which is how an MB6HC image 236 bytes too large was built.

set -u
cross=$1; build=$2; shift 2
mkdir -p "$build"
failures=0; checks=0

check() {	# name, condition result (0 = ok), detail
	checks=$((checks + 1))
	if [ "$2" -eq 0 ]; then echo "  ok    $1"; else echo "  FAIL  $1: $3"; failures=$((failures + 1)); fi
}

link() {	# script, text bytes, data bytes, output name; prints the linker output, returns its exit code
	local out=$build/$4
	printf '\t.section .text.fill,"ax",%%progbits\n\t.space %d, 0xA5\n\t.section .data.fill,"aw",%%progbits\n\t.space %d, 0x5A\n' "$2" "$3" > "$out.s"
	"${cross}as" -o "$out.o" "$out.s" || return 2
	"${cross}ld" -T "$1" -o "$out.elf" "$out.o" --no-warn-rwx-segments 2>&1
}

symbol() {	# elf, name
	local hex
	hex=$("${cross}nm" "$1" | awk -v n="$2" '$3 == n { print $1 }')
	[ -n "$hex" ] && echo $((16#$hex))
}

for script in "$@"; do
	name=$(basename "$script" .ld)
	echo "$name"
	# The end of rom from the MEMORY command, e.g. "rom (rx)  : ORIGIN = 0x00400000, LENGTH = 0x00100000"
	read -r origin length < <(awk '$1 == "rom" { gsub(/[,]/, " "); for (i = 1; i <= NF; i++) { if ($i == "ORIGIN") o = $(i + 2); if ($i == "LENGTH") l = $(i + 2) } print o, l; exit }' "$script")
	origin=$((origin)); length=$((length))
	if [ -z "${origin:-}" ] || [ -z "${length:-}" ]; then check "rom region found" 1 "no rom line in $script"; continue; fi
	romEnd=$((origin + length))
	data=256

	# Probe with a small image to find how far _firmware_crc moves with the code size
	probeText=4096
	if ! out=$(link "$script" $probeText $data "$name-probe"); then check "probe links" 1 "$out"; continue; fi
	crc=$(symbol "$build/$name-probe.elf" _firmware_crc)
	fitText=$((probeText + (romEnd - 4 - crc) / 8 * 8))

	out=$(link "$script" $fitText $data "$name-fits"); rc=$?
	crcFit=$(symbol "$build/$name-fits.elf" _firmware_crc 2>/dev/null)
	check "an image whose CRC ends $((romEnd - ${crcFit:-0} - 4)) bytes before the end of rom links" $rc "$out"
	[ -n "${crcFit:-}" ] && check "that image is within 8 bytes of the limit" $(( (crcFit + 4 <= romEnd && crcFit + 4 + 8 > romEnd) ? 0 : 1 )) "_firmware_crc = $crcFit, end of rom = $romEnd"

	out=$(link "$script" $((fitText + 8)) $data "$name-crc"); rc=$?
	check "an image 8 bytes larger, whose CRC ends beyond rom, is refused" $(( (rc != 0) && $(grep -c 'overflowed flash memory' <<< "$out") > 0 ? 0 : 1 )) "linker exit $rc: ${out:-no message}"

	# Code ending 64 bytes before the end of rom, with more initialised data than that, as in the MB6HC image
	out=$(link "$script" $((fitText - 64 + 4 + data)) 256 "$name-data"); rc=$?
	etext=$(symbol "$build/$name-data.elf" _etext 2>/dev/null)
	check "an image whose .relocate load image crosses the end of rom is refused" $(( (rc != 0) && $(grep -c 'overflowed flash memory' <<< "$out") > 0 ? 0 : 1 )) "linker exit $rc, _etext = ${etext:-?}, end of rom = $romEnd: ${out:-no message}"
done

echo "$((checks - failures)) of $checks checks passed"
[ $failures -eq 0 ]
