# Copies HeightController::Configure out of src/Movement/HeightControl/HeightController.cpp with a #line directive, so that the test
# compiles the real text and compiler messages point at the real source lines. Fails if it is missing, so a renamed function cannot
# silently drop out of the test. Give the file with an absolute path.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^GCodeResult HeightController::Configure\(/ {
	inFunc = 1
	printf "#line %d \"%s\"\n", FNR, FILENAME
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found = 1 }
}

END {
	if (!found) { print "HeightController::Configure not found" > "/dev/stderr"; exit 1 }
}
