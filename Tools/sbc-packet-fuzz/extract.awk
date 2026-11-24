# Copies function definitions, cases of the switch over SbcRequest, or an if or for block out of an RRF source file, each with a #line directive,
# so that the test compiles the real text and compiler messages point at the real source lines. Fails if one of them is missing, so a
# renamed function cannot silently drop out of the test.
#
#   awk -v f=<path for #line> -v names="Class::Function ..." -f extract.awk <file>		the first definition of each function, from the head to the "}" in column 1
#   awk -v f=<path for #line> -v names="case:OpenFileResult" -f extract.awk <file>		"case SbcRequest::OpenFileResult:" and its block
#   awk -v f=<path for #line> -v names="if:fileOperationPending.load" -f extract.awk <file>	"if (fileOperationPending.load..." in a function body and its block
#   awk -v f=<path for #line> -v names="for:size_t~i~=~0;~i~<~transfer.PacketsToRead()" -f extract.awk <file>
#                                                                                        "for (size_t i = 0; i < transfer.PacketsToRead()..." in a function
#                                                                                        body and its block ("~" stands for a space)
#   awk -v f=<path for #line> -v names="line:size_t~OutputBuffer::cat(const~char~*_ecv_array~src,~size_t~len)" -f extract.awk <file>
#                                                                                        the function whose head line starts with that text, for one
#                                                                                        overload of several

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

BEGIN { numWanted = split(names, wanted, " ") }

inBlock {
	print
	if ($0 ~ endPattern) { inBlock = 0; found[current] = 1 }
	next
}

{
	for (i = 1; i <= numWanted; i++)
	{
		w = wanted[i]
		if (w in found) { continue }		# only the first definition, e.g. FileStore::Invalidate() and not the overload after it
		if (w ~ /^case:/)
		{
			if ($0 == "\t\tcase SbcRequest::" substr(w, 6) ":") { current = w; endPattern = "^\t\t}$" }
			else { continue }
		}
		else if (w ~ /^if:/)
		{
			if (index($0, "\tif (" substr(w, 4)) == 1) { current = w; endPattern = "^\t}$" }
			else { continue }
		}
		else if (w ~ /^line:/)
		{
			text = substr(w, 6); gsub(/~/, " ", text)
			if (index($0, text) == 1 && $0 !~ /;[ \t]*$/) { current = w; endPattern = "^}" }
			else { continue }
		}
		else if (w ~ /^for:/)
		{
			text = substr(w, 5); gsub(/~/, " ", text)
			if (index($0, "\tfor (" text) == 1) { current = w; endPattern = "^\t}$" }
			else { continue }
		}
		else if ($0 ~ /^[A-Za-z]/ && index($0, w "(") > 0 && $0 !~ /;[ \t]*$/)
		{
			current = w; endPattern = "^}"
		}
		else { continue }
		inBlock = 1
		printf "#line %d \"%s\"\n", NR, f
		print
		break
	}
}

END {
	for (i = 1; i <= numWanted; i++)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found in " f > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
