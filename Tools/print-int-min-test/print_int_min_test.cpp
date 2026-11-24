// Host test of the integer conversions of RRFLibraries SafeVsnprintf (src/General/SafeVsnprintf.cpp, compiled unchanged):
// FormattedPrinter::PrintI negated a negative int with -i and FormattedPrinter::PrintLL a negative long long with -i, which is undefined
// behaviour for INT_MIN and LLONG_MIN. RRF prints the G-code number and the integer parameters of a binary code with %d
// (BinaryParser.cpp, finding B6 of Tools/binary-code-fuzz). See README.md.

#include <climits>
#include <cstdio>
#include <cstring>
#include <General/SafeVsnprintf.h>

static unsigned int runs = 0, failures = 0;

static void Check(const char *what, const char *got, const char *expected) noexcept
{
	++runs;
	const bool ok = (strcmp(got, expected) == 0);
	if (ok)
	{
		printf("PASS %s: \"%s\"\n", what, got);
	}
	else
	{
		printf("FAIL %s: \"%s\", expected \"%s\"\n", what, got, expected);
		++failures;
	}
}

int main()
{
	char buf[64];

	// PrintI: %d and %i
	SafeSnprintf(buf, sizeof(buf), "%d", INT_MIN);					Check("%d INT_MIN", buf, "-2147483648");
	SafeSnprintf(buf, sizeof(buf), "%i", INT_MIN);					Check("%i INT_MIN", buf, "-2147483648");
	SafeSnprintf(buf, sizeof(buf), "%d", INT_MIN + 1);				Check("%d INT_MIN + 1", buf, "-2147483647");
	SafeSnprintf(buf, sizeof(buf), "%d", -1);						Check("%d -1", buf, "-1");
	SafeSnprintf(buf, sizeof(buf), "%d", INT_MAX);					Check("%d INT_MAX", buf, "2147483647");
	SafeSnprintf(buf, sizeof(buf), "%d", 0);						Check("%d 0", buf, "0");
	SafeSnprintf(buf, sizeof(buf), "%u", 2147483648u);				Check("%u 2147483648", buf, "2147483648");
	SafeSnprintf(buf, sizeof(buf), "%x", 0x80000000u);				Check("%x 0x80000000", buf, "80000000");
	SafeSnprintf(buf, sizeof(buf), "M%d X%d", INT_MIN, -5);			Check("M%d X%d", buf, "M-2147483648 X-5");

	// PrintLL: %lld and %llu
	SafeSnprintf(buf, sizeof(buf), "%lld", LLONG_MIN);				Check("%lld LLONG_MIN", buf, "-9223372036854775808");
	SafeSnprintf(buf, sizeof(buf), "%lld", LLONG_MIN + 1);			Check("%lld LLONG_MIN + 1", buf, "-9223372036854775807");
	SafeSnprintf(buf, sizeof(buf), "%lld", -1LL);					Check("%lld -1", buf, "-1");
	SafeSnprintf(buf, sizeof(buf), "%lld", LLONG_MAX);				Check("%lld LLONG_MAX", buf, "9223372036854775807");
	SafeSnprintf(buf, sizeof(buf), "%llu", 9223372036854775808ull);	Check("%llu 2^63", buf, "9223372036854775808");

	printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
