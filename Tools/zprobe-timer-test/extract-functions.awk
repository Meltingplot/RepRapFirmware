# Copies whole function definitions, or a whole class declaration, out of a source file, each with a #line directive, so that
# the test compiles the real text and compiler messages point at the real source lines. Fails if one of them is missing, so a
# renamed function cannot silently drop out of the test.
#
#   awk -v f=<source path for #line> -v cls=<"Class::" or ""> -v names="<name> <name> ..." -f extract-functions.awk <source>
#   awk -v f=<source path for #line> -v decl=<Class> -f extract-functions.awk <header>
#
# A function definition starts in column 0, with or without a return type (constructors and destructors), and ends at the first
# line that starts with "}". All overloads of a name are copied. A class declaration starts with "class <Class>" in column 0 and
# ends at the first line that starts with "};".

BEGIN {
	if (decl != "")
	{
		n = 1
		wanted[1] = decl
		start = "^class " decl "([ \t:]|$)"
		finish = "^};"
	}
	else
	{
		n = split(names, wanted, " ")
		alternatives = ""
		for (i = 1; i <= n; ++i)
		{
			alternatives = alternatives ((i == 1) ? "" : "|") wanted[i]
		}
		start = "^([^ \t;][^;]*[ *&])?" cls "(" alternatives ")\\("
		finish = "^}"
	}
}

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

!inFunc && $0 ~ start && $0 !~ /^\/\// && $0 !~ /;[ \t]*$/ {
	inFunc = 1
	if (decl != "")
	{
		name = decl
	}
	else
	{
		name = $0
		sub(/\(.*/, "", name)
		sub(/.*[ *&:]/, "", name)
	}
	printf "#line %d \"%s\"\n", NR, f
}

inFunc {
	print
	if ($0 ~ finish) { inFunc = 0; found[name] = 1 }
}

END {
	for (i = 1; i <= n; ++i)
	{
		if (!(wanted[i] in found)) { print ((decl != "") ? "class " : cls) wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
