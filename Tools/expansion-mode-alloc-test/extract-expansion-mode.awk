# Copies DeleteObject out of src/RepRapFirmware.h, CanInterface::SwitchToExpansionMode out of src/CAN/CanInterface.cpp,
# GCodes::SwitchToExpansionMode out of src/GCodes/GCodes3.cpp and FilamentMonitor::DeleteAll out of
# src/FilamentMonitors/FilamentMonitor.cpp, each with a #line directive, so that the test compiles the real text and compiler
# messages point at the real source lines. Fails if one of them is missing, so a renamed function cannot silently drop out of the
# test. Give the files with absolute paths, RepRapFirmware.h first, because DeleteAll uses DeleteObject.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^template <typename T> void DeleteObject\(/ { Start("DeleteObject") }
/^void CanInterface::SwitchToExpansionMode\(/ { Start("CanInterface::SwitchToExpansionMode") }
/^void GCodes::SwitchToExpansionMode\(\)/ { Start("GCodes::SwitchToExpansionMode") }
/^\/\*static\*\/ void FilamentMonitor::DeleteAll\(\)/ { Start("FilamentMonitor::DeleteAll") }

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
	split("DeleteObject CanInterface::SwitchToExpansionMode GCodes::SwitchToExpansionMode FilamentMonitor::DeleteAll", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
