# Copies the definition of Move::laserTask and Move::CreateLaserTask out of src/Movement/Move.cpp, each with a #line directive, so
# that the test compiles the real text and compiler messages point at the real source lines. Fails if one of them is missing, so a
# renamed function cannot silently drop out of the test. Give the file with an absolute path.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^Task<Move::LaserTaskStackWords> \*_ecv_null Move::laserTask = / {
	printf "#line %d \"%s\"\n", FNR, FILENAME
	print
	found["Move::laserTask"] = 1
	next
}

/^void Move::CreateLaserTask\(\)/ {
	inFunc = 1
	name = "Move::CreateLaserTask"
	printf "#line %d \"%s\"\n", FNR, FILENAME
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found[name] = 1 }
}

END {
	split("Move::laserTask Move::CreateLaserTask", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
