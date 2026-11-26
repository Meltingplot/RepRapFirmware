# Copies a part of a real source file into gen/ with #line directives, so that the test compiles the real text and compiler messages and
# assertion reports point at the real file and line. Fails if the part is not found, so a change of the source cannot drop it silently.
#
#   awk -v f=<path for #line> -v start=<regex> -v end=<regex> [-v endafter=<regex>] [-v endexcl=1] -f extract.awk <file>
#       from the first line matching start to the first later line matching end (if endafter is given: the first line matching end
#       after a line matching endafter); endexcl=1 leaves the end line out
#   awk -v f=<path for #line> -v hook=<regex> -v text=<code> -f extract.awk <file>
#       the whole file, with <code> inserted before the first line matching hook (the line keeps its number)
#   awk -v f=<path for #line> -v start=<regex> -v end=<regex> -v hook=<regex> -v text=<code> -f extract.awk <file>
#       the part as above, with <code> inserted before the first line of the part that matches hook

{ sub(/\r$/, "") }

function Hook()
{
	if (hook != "" && !hooked && $0 ~ hook)
	{
		printf "%s\t/* inserted for the test before %s:%d */\n", text, f, NR
		printf "#line %d \"%s\"\n", NR, f
		hooked = NR
	}
}

BEGIN { if (hook != "" && start == "") { printf "#line 1 \"%s\"\n", f } }

start == "" {
	Hook()
	print
	next
}

!inPart && !done && $0 ~ start {
	inPart = 1
	sawAfter = (endafter == "")
	printf "#line %d \"%s\"\n", NR, f
	print
	next
}

inPart {
	if (!sawAfter && $0 ~ endafter) { sawAfter = 1 }
	if (sawAfter && $0 ~ end)
	{
		if (!endexcl) { print }
		inPart = 0
		done = NR
		next
	}
	Hook()
	print
}

END {
	if (start != "" && !done) { print "part /" start "/ .. /" end "/ not found in " f > "/dev/stderr"; exit 1 }
	if (hook != "")
	{
		if (!hooked) { print "hook line /" hook "/ not found in " f > "/dev/stderr"; exit 1 }
		print "inserted the hook before line " hooked " of " f > "/dev/stderr"
	}
}
