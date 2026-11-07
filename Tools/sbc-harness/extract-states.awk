# Turns the enumerators of DataTransfer::InternalTransferState into a table of names, so state= prints the names the
# real header has. Fails on an explicit enumerator value, because the table index would then no longer be the value.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/enum class InternalTransferState/ { inEnum = 1; next }

inEnum && /^[ \t]*\{/ { next }

inEnum && /\}/ { inEnum = 0; done = 1; exit }

inEnum {
	line = $0
	sub(/\/\/.*/, "", line)
	gsub(/[ \t,]/, "", line)
	if (line == "") next
	if (line ~ /=/) { print "explicit enumerator value: " line > "/dev/stderr"; exit 1 }
	printf "\"%s\",\n", line
	count++
}

END {
	if (!done || count == 0) { print "InternalTransferState not found" > "/dev/stderr"; exit 1 }
}
