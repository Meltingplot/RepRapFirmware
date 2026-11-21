# Copies the driver data accessors of ExpansionManager out of ExpansionManager.cpp, each with a #line directive, so that the test
# compiles the real text and compiler messages point at the real source lines. Fails if one of them is missing, so a renamed
# function cannot silently drop out of the test.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^[A-Za-z]+ ExpansionManager::(StoreDriverDirection|StoreDriverMode|GetDriverDirection|GetDriverMode)\(/ {
	inFunc = 1
	name = $0
	sub(/\(.*/, "", name)
	sub(/.*::/, "", name)
	printf "#line %d \"%s\"\n", NR, f
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found[name] = 1 }
}

END {
	split("StoreDriverDirection StoreDriverMode GetDriverDirection GetDriverMode", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print "ExpansionManager::" wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
