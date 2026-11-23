// Host test of EraseAndReset (CoreN2G src/SAM4S_4E_E70/Reset.cpp), which M999 P"ERASE" runs on the SAME70 through SoftwareReset
// (src/Hardware/ExceptionHandlers.cpp). EraseAndReset, Flash::Unlock and its two address helpers are the real text, extracted from
// CoreN2G by the Makefile. The flash controller, the watchdog and the reset controller are modelled here. See README.md.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <csetjmp>
#include <set>
#include <string>

// ---------------------------------------------------------------------------------------------------------------------------------
// The SAME70Q20B of Duet3_MB6HC and Duet3_MB6XD (Makefiles/Duet3_MB6XD.mk: __SAME70Q20B__): CoreN2G
// src/SAM4S_4E_E70/asf/sam/utils/cmsis/same70/include/same70q20b.h:675-685

#define SAME70 1
#define IFLASH_ADDR					(0x00400000u)
#define IFLASH_SIZE					(0x100000u)
#define IFLASH_PAGE_SIZE			(512u)
#define IFLASH_LOCK_REGION_SIZE		(8192u)
#define IFLASH_NB_OF_PAGES			(2048u)
#define _ecv_null

constexpr uint32_t NumLockRegions = IFLASH_SIZE / IFLASH_LOCK_REGION_SIZE;
constexpr uint32_t PagesPerRegion = IFLASH_LOCK_REGION_SIZE / IFLASH_PAGE_SIZE;

// ASF compiler.h:204, without _ASSERT_ENABLE_ as in the firmware builds
#define Assert(expr) ((void) 0)

// ASF efc.h:52-58 and :78
enum { EFC_RC_OK = 0, EFC_RC_ERROR = 1 };
#define EFC_FCMD_CLB	0x09

struct Efc { };
struct Wdt { };
struct Rstc { };
static Efc efc;
static Wdt wdt;
static Rstc rstc;
#define EFC		(&efc)
#define WDT		(&wdt)
#define RSTC	(&rstc)

namespace Flash
{
	bool Unlock(uint32_t start, uint32_t length) noexcept;
	bool ClearGpNvm(uint32_t gpnvm) noexcept;
}

uint32_t lastFlashError = 0;

// ---------------------------------------------------------------------------------------------------------------------------------
// The model. How the EEFC answers a Clear Lock Bit command for a page past the end of the flash is not in what this test rests on,
// so each case runs twice: once with an error for such a page, once with success.

static bool outOfRangeFails;
static bool interruptsDisabled;
static bool commandWithInterruptsOn;
static uint32_t commandsSinceKick, maxCommandsPerKick, totalCommands, kicks;
static std::set<uint32_t> regionsUnlocked;
static int clearedGpnvm;
static std::jmp_buf resetDone;

static void IrqDisable() noexcept { interruptsDisabled = true; }

uint32_t efc_perform_command(Efc *p_efc, uint32_t ul_command, uint32_t ul_argument) noexcept
{
	if (!interruptsDisabled)
	{
		commandWithInterruptsOn = true;
	}
	++totalCommands;
	if (++commandsSinceKick > maxCommandsPerKick)
	{
		maxCommandsPerKick = commandsSinceKick;
	}
	if (ul_command != EFC_FCMD_CLB)
	{
		return EFC_RC_ERROR;
	}
	if (ul_argument >= IFLASH_NB_OF_PAGES)
	{
		return (outOfRangeFails) ? EFC_RC_ERROR : EFC_RC_OK;
	}
	regionsUnlocked.insert(ul_argument / PagesPerRegion);			// CLB unlocks the lock region that holds the page
	return EFC_RC_OK;
}

static void wdt_restart(Wdt *) noexcept
{
	++kicks;
	commandsSinceKick = 0;
}

bool Flash::ClearGpNvm(uint32_t gpnvm) noexcept
{
	clearedGpnvm = (int)gpnvm;
	return true;
}

[[noreturn]] static void rstc_start_software_reset(Rstc *) noexcept
{
	std::longjmp(resetDone, 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The real code

#include "Flash.inc"

extern "C" {
#include "Reset.inc"
}

// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int failures = 0;

static void Check(bool ok, const std::string& name, const char *what, uint32_t value) noexcept
{
	if (!ok)
	{
		std::printf("FAIL %s: %s (%u)\n", name.c_str(), what, (unsigned int)value);
		++failures;
	}
}

static void RunCase(bool outOfRangeFailsArg) noexcept
{
	const std::string name = (outOfRangeFailsArg) ? "page past the end gives an error" : "page past the end gives success";
	outOfRangeFails = outOfRangeFailsArg;
	interruptsDisabled = commandWithInterruptsOn = false;
	commandsSinceKick = maxCommandsPerKick = totalCommands = kicks = 0;
	regionsUnlocked.clear();
	clearedGpnvm = -1;
	const unsigned int failuresBefore = failures;

	if (setjmp(resetDone) == 0)
	{
		EraseAndReset();
	}

	// SAM-BA can erase only unlocked regions, so all of them must be unlocked before the reset
	Check(regionsUnlocked.size() == NumLockRegions, name, "lock regions unlocked, expected all 128", (uint32_t)regionsUnlocked.size());
	Check(clearedGpnvm == 1, name, "GPNVM bit cleared before the reset, expected 1", (uint32_t)clearedGpnvm);
	Check(!commandWithInterruptsOn, name, "a flash command ran with interrupts on", 0);
	// The loop kicks the watchdog once per page, so the work between two kicks must stay small: one command for the lock region of
	// the page, and one more when the page is the last of its region (Flash::Unlock rounds the end up to the next region)
	Check(maxCommandsPerKick <= 2, name, "flash commands between two watchdog kicks, expected at most 2", maxCommandsPerKick);
	Check(totalCommands <= IFLASH_NB_OF_PAGES + NumLockRegions + 1, name, "flash commands in all, expected at most 2177", totalCommands);
	std::printf("%s %s: %u commands, at most %u between two of %u watchdog kicks\n", (failures == failuresBefore) ? "PASS" : "FAIL",
				name.c_str(), (unsigned int)totalCommands, (unsigned int)maxCommandsPerKick, (unsigned int)kicks);
}

int main()
{
	RunCase(true);
	RunCase(false);
	std::printf("%u failed\n", failures);
	return (failures == 0) ? 0 : 1;
}
