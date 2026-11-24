// Host test of the direct reads of SerialCDC (CoreN2G src/SerialCDC_tusb.cpp) on the SAME70 data cache.
// The class and its direct read functions are the real text, extracted from CoreN2G by the Makefile. The Cortex-M7 data cache, the
// USB DMA and the TinyUSB calls are modelled here. See README.md.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------------------------------------------------------------
// What the extracted code needs from Core.h, RTOSIface.h and Stream.h

#define SAME70 1
#define RTOS 1
#define _ecv_array
#define _ecv_null

typedef uint16_t Pin;
constexpr Pin NoPin = 0xFFFF;
typedef void *TaskHandle;

class Stream
{
public:
	virtual ~Stream() {}
	virtual int available() noexcept = 0;
	virtual int read() noexcept = 0;
	virtual size_t readBytes(char *buffer, size_t length) noexcept = 0;
	virtual void flush() noexcept = 0;
	virtual size_t write(uint8_t) noexcept = 0;
	virtual size_t write(const uint8_t *buffer, size_t size) noexcept = 0;
	virtual size_t canWrite() noexcept = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Model of the Cortex-M7 data cache: write-back, read-allocate and write-allocate, 32-byte lines. DMA writes go to RAM, not through
// the cache. The model counts dirty lines that an invalidate throws away, because that loses data that the processor wrote.

constexpr size_t LineSize = 32;

struct CacheLine
{
	uint8_t data[LineSize];
	bool dirty;
};

static std::map<uintptr_t, CacheLine> cache;
static unsigned int dirtyLinesDiscarded = 0;

static uintptr_t LineOf(uintptr_t addr) noexcept { return addr & ~(uintptr_t)(LineSize - 1); }

static CacheLine& Fill(uintptr_t line) noexcept
{
	auto it = cache.find(line);
	if (it == cache.end())
	{
		CacheLine l;
		memcpy(l.data, reinterpret_cast<const void *>(line), LineSize);
		l.dirty = false;
		it = cache.emplace(line, l).first;
	}
	return it->second;
}

static uint8_t CpuRead(const uint8_t *p) noexcept
{
	const uintptr_t a = reinterpret_cast<uintptr_t>(p);
	return Fill(LineOf(a)).data[a - LineOf(a)];
}

static void CpuWrite(uint8_t *p, uint8_t val) noexcept
{
	const uintptr_t a = reinterpret_cast<uintptr_t>(p);
	CacheLine& l = Fill(LineOf(a));
	l.data[a - LineOf(a)] = val;
	l.dirty = true;
}

// A speculative read of Normal memory: the line is filled with what RAM holds now
static void SpeculativeFill(const uint8_t *p) noexcept
{
	(void)Fill(LineOf(reinterpret_cast<uintptr_t>(p)));
}

// CMSIS 5.4.0 core_cm7.h: the maintenance loops start at addr and step by 32 while the size left is positive; DCIMVAC and DCCIMVAC
// act on the line that contains the address
static void SCB_CleanInvalidateDCache_by_Addr(uint32_t *addr, int32_t dsize) noexcept
{
	uintptr_t a = reinterpret_cast<uintptr_t>(addr);
	for (int32_t left = dsize; left > 0; left -= (int32_t)LineSize, a += LineSize)
	{
		auto it = cache.find(LineOf(a));
		if (it != cache.end())
		{
			if (it->second.dirty)
			{
				memcpy(reinterpret_cast<void *>(LineOf(a)), it->second.data, LineSize);
			}
			cache.erase(it);
		}
	}
}

static unsigned int invalidateCalls = 0;

static void SCB_InvalidateDCache_by_Addr(uint32_t *addr, int32_t dsize) noexcept
{
	++invalidateCalls;
	uintptr_t a = reinterpret_cast<uintptr_t>(addr);
	for (int32_t left = dsize; left > 0; left -= (int32_t)LineSize, a += LineSize)
	{
		auto it = cache.find(LineOf(a));
		if (it != cache.end())
		{
			if (it->second.dirty)
			{
				++dirtyLinesDiscarded;
			}
			cache.erase(it);
		}
	}
}

struct ScbModel { uint32_t CCR; };
[[maybe_unused]] static ScbModel scbModel = { 1u << 16 };	// data cache enabled (only the fixed SerialCDC reads it)
#define SCB (&scbModel)
#define SCB_CCR_DC_Msk (1ul << 16)

// ---------------------------------------------------------------------------------------------------------------------------------
// TinyUSB as SerialCDC uses it on the SAME70 (LibTinyusb src/tusb_config.h: two CDC interfaces, 512-byte RX FIFO at high speed)

#define CFG_TUD_CDC				(2)
#define CFG_TUD_CDC_RX_BUFSIZE	(512)

enum tusb_speed_t { TUSB_SPEED_FULL = 0, TUSB_SPEED_LOW = 1, TUSB_SPEED_HIGH = 2 };

static uint32_t fifoLevel = CFG_TUD_CDC_RX_BUFSIZE;		// the CDC stream has filled its FIFO and stopped
static bool endpointClaimed = false;
static uint8_t *xferBuffer = nullptr;
static uint16_t xferLength = 0;

static bool tud_inited() noexcept { return true; }
static tusb_speed_t tud_speed_get() noexcept { return TUSB_SPEED_HIGH; }
static void delay(uint32_t) noexcept { }
static uint32_t tud_cdc_n_available(uint8_t) noexcept { return fifoLevel; }
static uint32_t tud_cdc_n_read(uint8_t, void *, uint32_t) noexcept { return 0; }
static bool usbd_edpt_claim(uint8_t, uint8_t) noexcept { endpointClaimed = true; return true; }
static bool usbd_edpt_release(uint8_t, uint8_t) noexcept { endpointClaimed = false; return true; }

// The DMA path of dcd_edpt_xfer in LibTinyusb src/tinyusb/src/portable/microchip/samx7x/dcd_samx7x.c: CleanInValidateCache over the
// buffer before the DMA starts
static bool usbd_edpt_xfer(uint8_t, uint8_t, uint8_t *buffer, uint16_t total_bytes) noexcept
{
	xferBuffer = buffer;
	xferLength = total_bytes;
	SCB_CleanInvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(buffer) & ~(uintptr_t)3), total_bytes + 31);
	return true;
}

// The firmware's FreeRTOS assert hook. Only called in a child process, see RunContractCase
extern "C" [[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept
{
	_exit(3);
}

constexpr uint32_t DCacheLineSize = 32;		// defined in SerialCDC_tusb.cpp next to the functions

static unsigned int numRunningCdcInterfaces = 0;

#include "SerialCDCClass.inc"

static SerialCDC *serialCDCInstances[CFG_TUD_CDC] = { nullptr };

#include "SerialCDC.inc"

// The stream functions are not part of the test, but the class needs them
int SerialCDC::available() noexcept { return 0; }
int SerialCDC::read() noexcept { return -1; }
size_t SerialCDC::readBytes(char *, size_t) noexcept { return 0; }
void SerialCDC::flush() noexcept { }
size_t SerialCDC::write(uint8_t) noexcept { return 1; }
size_t SerialCDC::write(const uint8_t *, size_t size) noexcept { return size; }
size_t SerialCDC::canWrite() noexcept { return 512; }

// ---------------------------------------------------------------------------------------------------------------------------------
// The memory: two 4096-byte direct buffers next to each other, as GCodeInput allocates them (Tasks::AllocPermanent, 32-byte aligned,
// downwards), with a line of other data before and after them

constexpr size_t BufferSize = 4096;
alignas(32) static uint8_t ram[LineSize + 2 * BufferSize + LineSize];
static uint8_t *const before = ram;
static uint8_t *const buffers[2] = { ram + LineSize + BufferSize, ram + LineSize };
static uint8_t *const after = ram + LineSize + 2 * BufferSize;

static uint8_t OldByte(size_t i) noexcept { return (uint8_t)(0xA5 ^ i); }
static uint8_t NewByte(size_t i) noexcept { return (uint8_t)(0x3C + 7 * i); }
static uint8_t OtherByte(size_t i) noexcept { return (uint8_t)(0x5A + i); }

enum class Speculation { none, firstLine, lastReceivedLine, lineAfterReceived, everyLine };
static const char *const SpeculationNames[] = { "none", "first line", "last received line", "line after the received data", "every line" };

static unsigned int runs = 0, failures = 0;

static void Fail(const std::string& name, const char *what, long offset = -1) noexcept
{
	++failures;
	if (offset >= 0)
	{
		printf("FAIL %s: %s at offset %ld\n", name.c_str(), what, offset);
	}
	else
	{
		printf("FAIL %s: %s\n", name.c_str(), what);
	}
}

// One direct read into buffers[whichBuffer] of 'length' bytes that the host ends after 'received' bytes, with the processor reading lines
// of the buffer speculatively while the DMA writes it
static void RunReadCase(unsigned int whichBuffer, size_t length, size_t received, Speculation spec) noexcept
{
	++runs;
	char nameBuf[160];
	snprintf(nameBuf, sizeof(nameBuf), "buffer %u, read %zu, received %zu, speculative fill: %s", whichBuffer, length, received, SpeculationNames[(int)spec]);
	const std::string name(nameBuf);
	const unsigned int failuresBefore = failures;

	cache.clear();
	dirtyLinesDiscarded = 0;
	invalidateCalls = 0;
	endpointClaimed = false;
	fifoLevel = CFG_TUD_CDC_RX_BUFSIZE;
	uint8_t *const buf = buffers[whichBuffer];
	for (size_t i = 0; i < sizeof(ram); ++i)
	{
		ram[i] = OldByte(i);
	}

	// The reader went through this buffer before (the previous upload chunk), so its lines are in the cache. The processor also wrote
	// the data next to the buffers, so those lines are dirty.
	for (size_t i = 0; i < BufferSize; ++i)
	{
		(void)CpuRead(buf + i);
	}
	for (size_t i = 0; i < LineSize; ++i)
	{
		CpuWrite(before + i, OtherByte(i));
		CpuWrite(after + i, OtherByte(i + LineSize));
	}

	SerialCDC cdc(0);
	cdc.Start(NoPin);
	if (!cdc.TakeReceiveEndpoint())
	{
		Fail(name, "TakeReceiveEndpoint failed");
		return;
	}
	if (!cdc.StartDirectRead(buf, length) || xferBuffer != buf || xferLength != length)
	{
		Fail(name, "StartDirectRead did not start the transfer");
		return;
	}

	// While the DMA runs
	switch (spec)
	{
	case Speculation::none:
		break;
	case Speculation::firstLine:
		SpeculativeFill(buf);
		break;
	case Speculation::lastReceivedLine:
		SpeculativeFill(buf + received - 1);
		break;
	case Speculation::lineAfterReceived:
		SpeculativeFill(buf + received);
		break;
	case Speculation::everyLine:
		for (size_t i = 0; i < length; i += LineSize)
		{
			SpeculativeFill(buf + i);
		}
		break;
	}
	for (size_t i = 0; i < received; ++i)
	{
		buf[i] = NewByte(i);				// the DMA stores into RAM
	}

	if (!cdc.DirectReadXferComplete(0x02, (uint32_t)received))		// EPNUM_CDC_0_OUT
	{
		Fail(name, "DirectReadXferComplete did not take the completion");
		return;
	}

	size_t bytesRead = 0;
	if (!cdc.IsDirectReadComplete(bytesRead) || bytesRead != received)
	{
		Fail(name, "IsDirectReadComplete did not report the received bytes");
		return;
	}

	for (size_t i = 0; i < received; ++i)
	{
		if (CpuRead(buf + i) != NewByte(i))
		{
			Fail(name, "the reader sees what the buffer held before the DMA", (long)i);
			break;
		}
	}

	if (dirtyLinesDiscarded != 0)
	{
		Fail(name, "a dirty cache line was invalidated, so data the processor wrote was lost");
	}
	for (size_t i = 0; i < LineSize; ++i)
	{
		if (CpuRead(before + i) != OtherByte(i) || CpuRead(after + i) != OtherByte(i + LineSize))
		{
			Fail(name, "the data next to the buffers changed", (long)i);
			break;
		}
	}

	size_t again;
	if (cdc.IsDirectReadComplete(again))
	{
		Fail(name, "IsDirectReadComplete reported the read twice");
	}

	if (failures == failuresBefore)
	{
		printf("PASS %s\n", name.c_str());
	}
}

// StartDirectRead with a buffer or length that is not a whole number of cache lines must stop with an assertion, because the invalidate
// at the end would throw away the other data in the shared line. Run in a child process, because vAssertCalled does not return.
static void RunContractCase(size_t offset, size_t length) noexcept
{
	++runs;
	char nameBuf[120];
	snprintf(nameBuf, sizeof(nameBuf), "StartDirectRead at buffer offset %zu, length %zu", offset, length);
	const std::string name(nameBuf);

	fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0)
	{
		cache.clear();
		SerialCDC cdc(0);
		cdc.Start(NoPin);
		(void)cdc.TakeReceiveEndpoint();
		(void)cdc.StartDirectRead(buffers[0] + offset, length);
		_exit(0);
	}

	int status = 0;
	(void)waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
	{
		printf("PASS %s: assertion\n", name.c_str());
	}
	else
	{
		Fail(name, "no assertion for a buffer that shares a cache line with other data");
	}
}

int main() noexcept
{
	for (unsigned int whichBuffer = 0; whichBuffer < 2; ++whichBuffer)
	{
		for (size_t length : { (size_t)512, (size_t)1024, BufferSize })
		{
			for (size_t received : { length, (size_t)300 })
			{
				for (Speculation spec : { Speculation::none, Speculation::firstLine, Speculation::lastReceivedLine, Speculation::lineAfterReceived, Speculation::everyLine })
				{
					if (spec == Speculation::lineAfterReceived && received == length)
					{
						continue;		// the line after a full buffer belongs to the other buffer or to the other data
					}
					RunReadCase(whichBuffer, length, received, spec);
				}
			}
		}
	}

	RunContractCase(16, 512);
	RunContractCase(4, 1024);
	RunContractCase(0, 500);

	printf("%u runs, %u failed\n", runs, failures);
	return (failures == 0) ? 0 : 1;
}
