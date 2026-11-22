# Copies the ReadWriteLock part of RRFLibraries src/RTOSIface/RTOSIface.cpp (LockRecord and the ReadWriteLock members, from the comment
# "Class ReadWriteLock members" up to "// End") with a #line directive, so that the test compiles the real text and compiler messages
# point at the real source lines. Fails if the part is missing, so that a change of the file cannot silently drop it out of the test.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^\/\/ Class ReadWriteLock members/ {
	inPart = 1
	printf "#line %d \"%s\"\n", NR, f
}

inPart && /^\/\/ End/ { inPart = 0; found = 1 }

inPart { print }

END {
	if (!found) { print "the ReadWriteLock members were not found in " f > "/dev/stderr"; exit 1 }
}
