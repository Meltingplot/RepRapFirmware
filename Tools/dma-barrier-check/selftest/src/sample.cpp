// Sample for the selftest of dma-barrier-check.py: starts of an XDMAC channel with and without a barrier on the way there. The
// functions do nothing useful, the selftest only reads their machine code. A comment "finding" marks each start that the check
// must report, with its key in selftest/expected.txt.

#include <cstdint>

#define XDMAC_GE	(*reinterpret_cast<volatile uint32_t *>(0x4007801C))		// XDMAC global channel enable register of the SAME70

alignas(32) volatile uint8_t buffer[64];
volatile bool fail;

static inline __attribute__((always_inline)) void Barrier() noexcept { __asm volatile ("dsb sy" ::: "memory"); }

// The check takes an ELF without this function for some other firmware
extern "C" [[gnu::noinline]] void xdmac_configure_transfer() noexcept { __asm volatile ("" ::: "memory"); }

// Does not return, so a call to it ends the path
extern "C" [[noreturn, gnu::noinline]] void vAssertCalled(uint32_t line) noexcept
{
	for (;;) { __asm volatile ("nop"); }
}

// A barrier on every path that returns, as Cache::Flush of CoreN2G on the SAME70 after the fix
[[gnu::noinline]] void Flush() noexcept
{
	if (fail)
	{
		vAssertCalled(__LINE__);
	}
	Barrier();
}

// No barrier, as Cache::Flush of CoreN2G on the SAME70 before the fix
[[gnu::noinline]] void FlushWithoutBarrier() noexcept
{
	if (fail)
	{
		vAssertCalled(__LINE__);
	}
}

// A barrier on one path only
[[gnu::noinline]] void SometimesBarrier(bool b) noexcept
{
	if (b)
	{
		Barrier();
	}
}

// A start that relies on its callers for the barrier, as setup_spi of src/SBC/DataTransfer.cpp
[[gnu::noinline]] void StartChannel(uint32_t channel) noexcept
{
	XDMAC_GE = 1u << channel;
}

[[gnu::noinline]] void UnflushedStart() noexcept			// finding
{
	buffer[0] = 1;
	FlushWithoutBarrier();
	StartChannel(1);
}

[[gnu::noinline]] void ConditionalBarrierStart(bool b) noexcept	// finding
{
	buffer[1] = 2;
	SometimesBarrier(b);
	StartChannel(2);
}

[[gnu::noinline]] void InlineStart() noexcept				// finding
{
	buffer[2] = 3;
	XDMAC_GE = 1u << 3;
}

[[gnu::noinline]] void BranchStart(bool b) noexcept			// finding
{
	buffer[3] = 4;
	if (b)
	{
		Flush();
	}
	XDMAC_GE = 1u << 4;
}

// Reached through a function pointer only
[[gnu::noinline]] void IndirectOnlyStart() noexcept			// finding
{
	buffer[4] = 5;
	XDMAC_GE = 1u << 5;
}

void (*volatile indirect)() noexcept = IndirectOnlyStart;

[[gnu::noinline]] void FlushedStart() noexcept
{
	buffer[5] = 6;
	Flush();
	StartChannel(6);
}

[[gnu::noinline]] void InlineBarrierStart() noexcept
{
	buffer[6] = 7;
	Barrier();
	XDMAC_GE = 1u << 7;
}

[[gnu::noinline]] void BothBranchesStart(bool b) noexcept
{
	buffer[7] = 8;
	if (b)
	{
		Flush();
	}
	else
	{
		Barrier();
	}
	XDMAC_GE = 1u << 8;
}

// The starts without a barrier come first: a barrier that Reset_Handler has run before a call orders the start in it (README.md)
extern "C" [[noreturn]] void Reset_Handler() noexcept
{
	xdmac_configure_transfer();
	UnflushedStart();
	ConditionalBarrierStart(fail);
	InlineStart();
	BranchStart(fail);
	indirect();
	FlushedStart();
	InlineBarrierStart();
	BothBranchesStart(fail);
	for (;;) { }
}
