# Copies whole definitions out of a source file, each with a #line directive, so that the test compiles the real text and compiler
# messages point at the real source lines. Fails if one of them is missing, so a renamed definition cannot silently drop out of
# the test.
#
#   awk -v f=<source path for #line> -v cls=<"Class::" or ""> -v names="<name> <name> ..." -f extract-definitions.awk <source>
#   awk -v f=<source path for #line> -v types="<name> <name> ..." -f extract-definitions.awk <source>
#
# A function definition starts in column 0 with the return type and ends at the first line that starts with "}".
# A type definition (enum class, union, struct or class) starts in column 0 and ends at the first line that starts with "};".

BEGIN {
	n = split(names, wanted, " ")
	alternatives = ""
	for (i = 1; i <= n; ++i)
	{
		alternatives = alternatives ((i == 1) ? "" : "|") wanted[i]
	}
	start = "^[A-Za-z][^;]*[ *&]" cls "(" alternatives ")\\("

	nt = split(types, wantedTypes, " ")
	typeAlternatives = ""
	for (i = 1; i <= nt; ++i)
	{
		typeAlternatives = typeAlternatives ((i == 1) ? "" : "|") wantedTypes[i]
	}
	typeStart = "^(enum class|union|struct|class) (" typeAlternatives ")([ :]|$)"
}

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

!inFunc && !inType && n > 0 && $0 ~ start && $0 !~ /;[ \t]*$/ {
	inFunc = 1
	name = $0
	sub(/\(.*/, "", name)
	sub(/.*[ *&:]/, "", name)
	printf "#line %d \"%s\"\n", NR, f
}

!inFunc && !inType && nt > 0 && $0 ~ typeStart && $0 !~ /;[ \t]*$/ {
	inType = 1
	name = $0
	sub(/^(enum class|union|struct|class) /, "", name)
	sub(/[ :].*/, "", name)
	printf "#line %d \"%s\"\n", NR, f
}

inFunc {
	print
	if ($0 ~ /^}/) { inFunc = 0; found[name] = 1 }
	next
}

inType {
	print
	if ($0 ~ /^};/) { inType = 0; found[name] = 1 }
}

END {
	for (i = 1; i <= n; ++i)
	{
		if (!(wanted[i] in found)) { print cls wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	for (i = 1; i <= nt; ++i)
	{
		if (!(wantedTypes[i] in found)) { print wantedTypes[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
