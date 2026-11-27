# Copies the code under test out of the RRF sources with a #line directive, so that the test compiles the real text and compiler messages
# point at the real source lines. Fails if a part is missing, so a renamed function or a moved block cannot silently drop out of the test.
#
#   awk -v part=enum -f extract.awk src/GCodes/GCodeInput.h		enum class GCodeInputState, to "};"
#   awk -v part=scanner -f extract.awk src/GCodes/GCodeInput.cpp	RegularGCodeInput::CheckForUrgentCommand, to the "}" in column 1
#   awk -v part=m24 -f extract.awk src/GCodes/GCodes2.cpp		the feed hold block of "case 24:" in GCodes::HandleMcode, from its
#									"if (reprap.GetMove().IsFeedHoldActive())" to the "}" at the same indent
#
# Give the file with an absolute path.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

part == "enum" && $0 == "enum class GCodeInputState" { inPart = 1; endPattern = "^};"; printf "#line %d \"%s\"\n", FNR, FILENAME }
part == "scanner" && /^bool RegularGCodeInput::CheckForUrgentCommand\(char c\) noexcept/ { inPart = 1; endPattern = "^}"; printf "#line %d \"%s\"\n", FNR, FILENAME }

# HandleMcode has two "case 24:" lines; the one in its main switch carries this comment
part == "m24" && $0 == "\t\t\tcase 24: // Print/resume-printing the selected file" { afterCase = 1; next }
afterCase {
	afterCase = 0
	if ($0 == "\t\t\t\tif (reprap.GetMove().IsFeedHoldActive())") { inPart = 1; endPattern = "^\t\t\t\t}$"; printf "#line %d \"%s\"\n", FNR, FILENAME }
}

inPart {
	print
	if ($0 ~ endPattern) { inPart = 0; found = 1; exit }
}

END {
	if (!found) { print part " not found in " FILENAME > "/dev/stderr"; exit 1 }
}
