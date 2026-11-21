# Copies DeleteObject out of RepRapFirmware.h, and Move::Exit, the definition of Move::laserTask and Move::WakeLaserTask out of
# Move.cpp, each with a #line directive, so that the test compiles the real text and compiler messages point at the real source
# lines. Fails if one of them is missing, so a renamed function cannot silently drop out of the test.
# Give the files with absolute paths, RepRapFirmware.h first, because Move::Exit may use DeleteObject.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^template <typename T> void DeleteObject\(/ { Start("DeleteObject") }
/^void Move::Exit\(\)/ { Start("Move::Exit") }
/^void Move::WakeLaserTask\(\)/ { Start("Move::WakeLaserTask") }

/^Task<Move::LaserTaskStackWords> \*_ecv_null Move::laserTask = / {
	printf "#line %d \"%s\"\n", FNR, FILENAME
	print
	found["Move::laserTask"] = 1
	next
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found[name] = 1 }
}

function Start(n)
{
	inFunc = 1
	name = n
	printf "#line %d \"%s\"\n", FNR, FILENAME
}

END {
	split("DeleteObject Move::Exit Move::laserTask Move::WakeLaserTask", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
