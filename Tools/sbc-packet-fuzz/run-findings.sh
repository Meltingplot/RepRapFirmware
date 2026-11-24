#!/bin/sh
# Checks the reproducers of the open findings: runs every <dir>/*.hex against <binary> -classify and compares the key of its sanitizer
# report with the "# expect:" line of the file. Exit code 1 if one of them no longer reports its finding (fixed, or changed).
#
#   sh run-findings.sh <binary> <dir>

bin=$1
dir=$2
fail=0
for f in "$dir"/*.hex; do
	expect=$(sed -n 's/^# expect: //p' "$f" | head -n 1)
	got=$("$bin" -classify "$f" | cut -f 1)
	if [ "$got" = "$expect" ]; then
		echo "reports  $(basename "$f"): $got"
	else
		echo "CHANGED  $(basename "$f"): expected \"$expect\", got \"$got\""
		fail=1
	fi
done
exit $fail
