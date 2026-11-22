# Copies DDARing::ConfigureMovementQueue out of src/Movement/DDARing.cpp with a #line directive, so that the test compiles the real
# text and compiler messages point at the real source lines. Fails if it is missing, so a renamed function cannot silently drop out
# of the test. Give the file with an absolute path.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^GCodeResult DDARing::ConfigureMovementQueue\(/ {
	inFunc = 1
	printf "#line %d \"%s\"\n", FNR, FILENAME
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found = 1 }
}

END {
	if (!found) { print "DDARing::ConfigureMovementQueue not found" > "/dev/stderr"; exit 1 }
}
