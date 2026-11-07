# Copies the SbcYieldTimeout constant and the whole SbcInterface::TaskLoop function out of SbcInterface.cpp, with #line
# directives, so that the harness compiles the real text and compiler messages point at the real source lines.
# Fails if either piece is missing, so a renamed function cannot silently drop out of the harness.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^constexpr uint32_t SbcYieldTimeout *=/ {
	printf "#line %d \"%s\"\n%s\n", NR, f, $0
	foundYield = 1
}

# Optional, so that sources from before the boot arm delay still build
/^constexpr uint32_t SbcBootArmDelay *=/ {
	printf "#line %d \"%s\"\n%s\n", NR, f, $0
}

/^\[\[noreturn\]\] void SbcInterface::TaskLoop\(\) noexcept/ {
	inLoop = 1
	printf "#line %d \"%s\"\n", NR, f
}

inLoop {
	print
	if ($0 ~ /^}/) { inLoop = 0; foundLoop = 1 }
}

END {
	if (!foundYield) { print "SbcYieldTimeout not found" > "/dev/stderr"; exit 1 }
	if (!foundLoop) { print "SbcInterface::TaskLoop not found" > "/dev/stderr"; exit 1 }
}
