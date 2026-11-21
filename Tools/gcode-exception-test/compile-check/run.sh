#!/bin/sh
# Compile check of GCodeExceptionMessage, called by the Makefile with the compiler command as its arguments.
# accept.cpp must compile. Each CASE of reject.cpp must fail, and every error must be on the line of that case, so that an
# unrelated error, for example in a header, does not count as a rejected message. See ../README.md.

dir=$(dirname "$0")
passed=0
failed=0

if "$@" -fsyntax-only "$dir/accept.cpp"; then
	echo "PASS  accept.cpp compiles"
	passed=$((passed + 1))
else
	echo "FAIL  accept.cpp does not compile"
	failed=$((failed + 1))
fi

for c in $(sed -n 's/^#\(el\)\{0,1\}if CASE == \([0-9]*\).*/\2/p' "$dir/reject.cpp"); do
	line=$(awk -v c="$c" '$0 ~ "^#(el)?if CASE == " c "([^0-9]|$)" { print NR + 1; exit }' "$dir/reject.cpp")
	what=$(awk -v c="$c" '$0 ~ "^#(el)?if CASE == " c "([^0-9]|$)" { sub(/.*\/\/ */, ""); print; exit }' "$dir/reject.cpp")
	if out=$("$@" -fsyntax-only -DCASE="$c" "$dir/reject.cpp" 2>&1); then
		echo "FAIL  case $c compiles: $what"
		failed=$((failed + 1))
	elif echo "$out" | grep 'error:' | grep -qv "reject.cpp:$line:"; then
		echo "FAIL  case $c fails with an error that is not on line $line: $what"
		echo "$out" | grep 'error:'
		failed=$((failed + 1))
	else
		echo "PASS  case $c is rejected: $what"
		passed=$((passed + 1))
	fi
done

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
