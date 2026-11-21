# Copies the functions that throw a GCodeException with the text of a reply buffer out of Heater.cpp, SwitchEndstop.cpp and
# CanInterface.cpp, each with a #line directive, so that the test compiles the real text and compiler messages point at the real
# source lines. Fails if one of them is missing, so a renamed function cannot silently drop out of the test.
# A file-scope reply buffer of CanInterface.cpp is copied too: RRF up to 3.7.0+1.mp.10 kept the reply of
# EnableRemoteStallEndstop in "static String<StringLength100> enableEndstopsReply", and copying it lets the test build against
# that source and against any upstream version that still has it.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^static String<[A-Za-z0-9]+> enableEndstopsReply;/ {
	printf "#line %d \"%s\"\n%s\n", FNR, FILENAME, $0
	next
}

/^void (Heater::SetTemperature|SwitchEndstop::PrimeAxis|CanInterface::EnableRemoteStallEndstop)\(/ {
	inFunc = 1
	name = $0
	sub(/\(.*/, "", name)
	sub(/^void /, "", name)
	printf "#line %d \"%s\"\n", FNR, FILENAME
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found[name] = 1 }
}

END {
	split("Heater::SetTemperature SwitchEndstop::PrimeAxis CanInterface::EnableRemoteStallEndstop", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
