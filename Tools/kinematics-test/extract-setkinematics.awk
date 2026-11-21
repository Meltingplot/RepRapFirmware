# Copies the whole Move::SetKinematics function out of Move.cpp, with a #line directive, so that the test compiles the real text
# and compiler messages point at the real source lines. Fails if the function is missing, so a renamed function cannot silently
# drop out of the test.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^bool Move::SetKinematics\(/ {
	inFunc = 1
	printf "#line %d \"%s\"\n", NR, f
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found = 1 }
}

END {
	if (!found) { print "Move::SetKinematics not found" > "/dev/stderr"; exit 1 }
}
