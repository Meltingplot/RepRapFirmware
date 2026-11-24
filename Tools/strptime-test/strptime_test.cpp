// Host test of RRF's SafeStrptime (src/libc/strptime.cpp, compiled unchanged): the month of %m indexes _DAYS_BEFORE_MONTH, so a month
// outside 1 to 12 must be refused. datetime() in expressions and M905 P pass user text to it (ExpressionParser.cpp, GCodes3.cpp). See
// README.md.

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

const char *SafeStrptime(const char *buf, const char *format, struct tm *timeptr) noexcept;	// as in src/RepRapFirmware.h

static unsigned int passed = 0, failed = 0;

static void Report(const std::string& name, const std::string& why) noexcept
{
	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name.c_str());
	}
	else
	{
		++failed;
		printf("FAIL %s:%s\n", name.c_str(), why.c_str());
	}
}

// A date that must be parsed, with the fields that SafeStrptime fills in
static void Valid(const char *text, const char *format, int year, int mon, int mday, int yday) noexcept
{
	tm t = {};
	std::string why;
	const char *const end = SafeStrptime(text, format, &t);
	if (end == nullptr)
	{
		why = " refused";
	}
	else
	{
		if (*end != 0) { why += " stopped before the end"; }
		if (t.tm_year != year - 1900) { why += " tm_year " + std::to_string(t.tm_year); }
		if (t.tm_mon != mon) { why += " tm_mon " + std::to_string(t.tm_mon); }
		if (t.tm_mday != mday) { why += " tm_mday " + std::to_string(t.tm_mday); }
		if (t.tm_yday != yday) { why += " tm_yday " + std::to_string(t.tm_yday); }
	}
	Report(std::string("\"") + text + "\" is parsed", why);
}

// A date whose month is outside 1 to 12 must be refused
static void Refused(const char *text, const char *format) noexcept
{
	tm t = {};
	const char *const end = SafeStrptime(text, format, &t);
	Report(std::string("\"") + text + "\" is refused", (end == nullptr) ? "" : " parsed, tm_mon " + std::to_string(t.tm_mon));
}

int main()
{
	const char *const dateTime = "%Y-%m-%dT%H:%M:%S";		// datetime() and the HTTP time (ExpressionParser.cpp, HttpResponder.cpp)
	const char *const date = "%Y-%m-%d";					// M905 P (GCodes3.cpp)

	Valid("2026-11-24T01:02:03", dateTime, 2026, 10, 24, 327);
	Valid("2026-01-01", date, 2026, 0, 1, 0);
	Valid("2026-12-31", date, 2026, 11, 31, 364);
	Valid("2024-12-31", date, 2024, 11, 31, 365);			// leap year
	Valid("2024-03-01", date, 2024, 2, 1, 60);

	Refused("2026-00-01", date);
	Refused("2026-13-01", date);							// the first month beyond _DAYS_BEFORE_MONTH
	Refused("2026-99-01T00:00:00", dateTime);
	Refused("2026--1-01", date);
	Refused("2026-2147483647-01", date);
	Refused("2026--2147483648-01", date);					// ret - 1 overflows

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
