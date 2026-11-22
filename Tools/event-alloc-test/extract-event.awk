# Copies the definitions of the static members of Event, the constructor of Event, Event::AddEvent, Event::AddEventV and
# Event::FinishedProcessing out of src/Platform/Event.cpp, each with a #line directive, so that the test compiles the real text and
# compiler messages point at the real source lines. Fails if one of them is missing, so a renamed function cannot silently drop out
# of the test. Give the file with an absolute path.

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

/^Event \*_ecv_null Event::eventsPending = / || /^unsigned int Event::events(Queued|Processed) = / {
	printf "#line %d \"%s\"\n", FNR, FILENAME
	print
	found[$0 ~ /eventsPending/ ? "Event::eventsPending" : ($0 ~ /eventsQueued/ ? "Event::eventsQueued" : "Event::eventsProcessed")] = 1
	next
}

/^inline Event::Event\(/ { Start("Event::Event") }
/^\/\*static\*\/ bool Event::AddEvent\(/ { Start("Event::AddEvent") }
/^\/\*static\*\/ bool Event::AddEventV\(/ { Start("Event::AddEventV") }
/^\/\*static\*\/ void Event::FinishedProcessing\(/ { Start("Event::FinishedProcessing") }

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
	split("Event::eventsPending Event::eventsQueued Event::eventsProcessed Event::Event Event::AddEvent Event::AddEventV Event::FinishedProcessing", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
