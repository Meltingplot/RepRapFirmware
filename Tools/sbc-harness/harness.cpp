// Open-loop replay of SPI exchanges against RRF's real SBC link code.
//
// Real code: SBC/DataTransfer.cpp and Storage/CRC32.cpp (separate objects, compiled unchanged) and the text of
// SbcInterface::TaskLoop (gen/TaskLoop.inc, extracted from SbcInterface.cpp). TaskLoop runs as a coroutine; it hands
// control back to the replay whenever it blocks in TaskBase::TakeIndexed.
//
// Harness code (not RRF): the SAME70 SPI1/XDMAC/NVIC/TfrRdy model below, the task notification, and SbcInterface's
// ExchangeData and InvalidateResources, which TaskLoop calls. See README.md for the protocol and the exact boundaries.

#include <ucontext.h>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>
#ifdef __SANITIZE_ADDRESS__
# include <sanitizer/common_interface_defs.h>
#endif

#include <HarnessShim.h>
#include <SBC/SbcMessageFormats.h>
#define private public				// the replay reads DataTransfer::state and resets DataTransfer's static SAME70 buffers
#include <SBC/DataTransfer.h>
#undef private

// Globals of DataTransfer.cpp
extern volatile bool dataReceived;
extern volatile bool transferReadyHigh;
extern std::atomic<unsigned int> spiTxUnderruns, spiRxOverruns;

namespace
{
	const char *const StateNames[] =
	{
#include <StateNames.inc>
	};

	struct Counters
	{
		unsigned int emergencyStops, resets, internalErrors, exchangeDataCalls, packetsWritten;
	} counters;

	unsigned int skipTransfer;			// NEW <skip>
	uint8_t underrunByte;				// NEW <underrun>
	std::vector<std::string> got;		// "dsf:N" seen by ExchangeData in the current step
	bool debugOutput = false;			// -d: RRF's own debugPrintf output to stderr
	bool traceOutput = false;			// -t: platform messages and harness events to stderr
	bool delayBlocks = false;			// -b: delay() blocks until T, so exchanges can be clocked during it
	bool inDelay = false;				// TaskLoop is blocked in delay()
	uint32_t virtualTime = 0;
	unsigned int notifyCount = 0;		// FreeRTOS notification count of the SBC task, index NotifyIndices::SbcInterface
	TaskBase *const dummyTaskHandle = reinterpret_cast<TaskBase *>(&notifyCount);

	// Hardware model
	struct XdmacChannel
	{
		bool enabled;
		uint32_t sa, da, ubc;
	};

	struct Hardware
	{
		bool spiEnabled;
		uint32_t spiImr;
		bool nvicEnabled;
		bool tfrRdy;
		XdmacChannel channels[24];
	} hw;

	// The LwIP PBUF pool of the MB6HC build: PBUF_POOL_SIZE (8 RX + 4 TX GMAC buffers + 4) pbufs of 16 + 1536 bytes, 24832 bytes in all
	alignas(16) char pbufArena[16 * 1552];
	size_t pbufUsed = 0;

	uint8_t *AddressToPointer(uint32_t addr) noexcept
	{
		return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(addr));
	}
}

Spi HarnessSpi;
Xdmac HarnessXdmac;
RepRap reprap;

void ESP_SPI_HANDLER() noexcept { }

// ---------------------------------------------------------------------------------------------------------------------
// SbcInterface: the members TaskLoop uses, as declared in SbcInterface.h

class CountingTransfer : public DataTransfer
{
public:
	void ResetConnection(bool fullReset) noexcept
	{
		++counters.resets;
		if (traceOutput) { fprintf(stderr, "trace: ResetConnection(%d)\n", (int)fullReset); }
		DataTransfer::ResetConnection(fullReset);
	}
};

class SbcInterface
{
public:
	[[noreturn]] void TaskLoop() noexcept;

	CountingTransfer transfer;
	volatile bool isConnected = false;
	TransferState state = TransferState::doingFullTransfer;
	uint32_t numDisconnects = 0, numTimeouts = 0, numSbcTimeouts = 0;
#if SUPPORTS_SBC_OVER_USB
	SerialCDC *pendingUsbDevice = nullptr;
	unsigned int usbDeviceIndex = 0;
#endif

	void ExchangeData() noexcept;
	void InvalidateResources() noexcept;
};

#if SUPPORTS_SBC_OVER_USB
// Static helper above TaskLoop in SbcInterface.cpp. Not reached: pendingUsbDevice is never set.
static void SendUsbInitMessage(SerialCDC *) noexcept { }
#endif

#include <TaskLoop.inc>
#line 117 "harness.cpp"

namespace
{
	alignas(SbcInterface) unsigned char sbcStorage[sizeof(SbcInterface)];
	SbcInterface *sbc = nullptr;

	// Coroutine that runs SbcInterface::TaskLoop
	ucontext_t mainContext, taskContext;
	alignas(16) char taskStack[1024 * 1024];
	uint32_t blockedTimeout = 0;		// timeout of the TakeIndexed call TaskLoop is blocked in
#ifdef __SANITIZE_ADDRESS__
	const void *mainStackBottom = nullptr;
	size_t mainStackSize = 0;
#endif

	void SwitchToTask() noexcept
	{
#ifdef __SANITIZE_ADDRESS__
		void *fakeStack = nullptr;
		__sanitizer_start_switch_fiber(&fakeStack, taskStack, sizeof(taskStack));
#endif
		swapcontext(&mainContext, &taskContext);
#ifdef __SANITIZE_ADDRESS__
		__sanitizer_finish_switch_fiber(fakeStack, nullptr, nullptr);
#endif
	}

	void SwitchToMain() noexcept
	{
#ifdef __SANITIZE_ADDRESS__
		void *fakeStack = nullptr;
		__sanitizer_start_switch_fiber(&fakeStack, mainStackBottom, mainStackSize);
#endif
		swapcontext(&taskContext, &mainContext);
#ifdef __SANITIZE_ADDRESS__
		__sanitizer_finish_switch_fiber(fakeStack, &mainStackBottom, &mainStackSize);
#endif
	}

	void TaskEntry() noexcept
	{
#ifdef __SANITIZE_ADDRESS__
		__sanitizer_finish_switch_fiber(nullptr, &mainStackBottom, &mainStackSize);
#endif
		sbc->TaskLoop();
	}

	// Write the message packet that belongs to transfer n, using DataTransfer's own writer for code replies
	void WriteHarnessPacket(unsigned int n) noexcept
	{
		if (n == skipTransfer)
		{
			return;
		}
		char text[24];
		snprintf(text, sizeof(text), "rrf:%u", counters.packetsWritten + 1);
		OutputBuffer buffer(text, strlen(text));
		OutputBuffer *response = &buffer;
		if (sbc->transfer.WriteCodeReply(MessageType::UsbMessage, response))
		{
			++counters.packetsWritten;
		}
		else if (traceOutput)
		{
			fprintf(stderr, "trace: no tx space for %s\n", text);
		}
	}

	// Record every "dsf:<digits>" inside the payload of a packet, clipped to the receive buffer
	void ScanForDsf(const char *data, size_t length) noexcept
	{
		const char *const bufferEnd = sbc->transfer.rxBuffer + SbcTransferBufferSize;
		if (data < sbc->transfer.rxBuffer || data >= bufferEnd)
		{
			return;
		}
		const char *const end = (length > (size_t)(bufferEnd - data)) ? bufferEnd : data + length;
		for (const char *p = data; p + 4 < end; ++p)
		{
			if (memcmp(p, "dsf:", 4) == 0 && isdigit((unsigned char)p[4]))
			{
				const char *q = p + 4;
				while (q < end && isdigit((unsigned char)*q)) { ++q; }
				got.emplace_back(p, q - p);
				p = q - 1;
			}
		}
	}
}

// Harness version of SbcInterface::ExchangeData: the packet loop has the shape of the real one (bound by PacketsToRead,
// stop on a null packet or an invalid request other than ActivateLinkCapabilities). ActivateLinkCapabilities is executed
// like the real case does it, every other packet is only scanned for "dsf:N" instead of executed.
// Then it writes the message packet of the next transfer.
void SbcInterface::ExchangeData() noexcept
{
	for (size_t i = 0; i < transfer.PacketsToRead(); i++)
	{
		const PacketHeader * const packet = transfer.ReadPacket();
		if (packet == nullptr)
		{
			break;
		}
		if (packet->request >= (uint16_t)SbcRequest::InvalidRequest && packet->request != (uint16_t)SbcRequest::ActivateLinkCapabilities)
		{
			REPORT_INTERNAL_ERROR;
			break;
		}
		const char * const data = transfer.ReadData(packet->length);
		if (packet->request == (uint16_t)SbcRequest::ActivateLinkCapabilities)
		{
			if (packet->length >= sizeof(uint32_t))
			{
				transfer.ActivateLinkCapabilities(*reinterpret_cast<const uint32_t *>(data));
			}
			if (traceOutput) { fprintf(stderr, "trace: link capabilities %" PRIu32 " active\n", transfer.GetActiveLinkCapabilities()); }
			continue;
		}
		ScanForDsf(data, packet->length);
	}

	++counters.exchangeDataCalls;
	WriteHarnessPacket(counters.exchangeDataCalls + 1);
}

// The real InvalidateResources does not touch DataTransfer (code buffer, files, G-code channels, heaters only)
void SbcInterface::InvalidateResources() noexcept
{
	if (traceOutput) { fprintf(stderr, "trace: InvalidateResources\n"); }
}

// ---------------------------------------------------------------------------------------------------------------------
// RTOS

bool TaskBase::TakeIndexed(uint32_t index, uint32_t timeout) noexcept
{
	if (index == NotifyIndices::SbcInterface && notifyCount != 0)
	{
		notifyCount = 0;
		return true;
	}

	// The yield after a connection reset: SPI is disarmed then (DoTransfer returned before setting up an exchange),
	// so no NSS interrupt can wake the task and the wait always runs out
	if (timeout == SbcYieldTimeout)
	{
		virtualTime += timeout;
		return false;
	}

	blockedTimeout = timeout;
	SwitchToMain();
	if (notifyCount != 0)
	{
		notifyCount = 0;
		return true;
	}
	virtualTime += (timeout == TimeoutUnlimited) ? 0 : timeout;
	return false;
}

void TaskBase::GiveFromISR(TaskBase *h, uint32_t index) noexcept
{
	if (h != nullptr && index == NotifyIndices::SbcInterface)
	{
		++notifyCount;
	}
}

TaskBase *TaskBase::GetCallerTaskHandle() noexcept
{
	return dummyTaskHandle;
}

uint32_t millis() noexcept { return virtualTime; }
// With -b, TaskLoop blocks in delay() until T. A notification given meanwhile stays pending, as with vTaskDelay
void delay(uint32_t ms) noexcept
{
	if (delayBlocks)
	{
		inDelay = true;
		SwitchToMain();
		inDelay = false;
	}
	virtualTime += ms;
}

// ---------------------------------------------------------------------------------------------------------------------
// SAME70 model

void spi_reset(Spi *) noexcept
{
	hw.spiEnabled = false;
	hw.spiImr = 0;
	HarnessSpi.SPI_SR.value = 0;
}

void spi_enable(Spi *) noexcept { hw.spiEnabled = true; }
void spi_disable(Spi *) noexcept { hw.spiEnabled = false; }
void HarnessSpiIer::operator=(uint32_t v) noexcept { hw.spiImr |= v; }
void HarnessSpiIdr::operator=(uint32_t v) noexcept { hw.spiImr &= ~v; }

void xdmac_configure_transfer(Xdmac *, uint32_t ch, xdmac_channel_config_t *cfg) noexcept
{
	hw.channels[ch].sa = cfg->mbr_sa;
	hw.channels[ch].da = cfg->mbr_da;
	hw.channels[ch].ubc = cfg->mbr_ubc;
}

void xdmac_channel_enable(Xdmac *, uint32_t ch) noexcept { hw.channels[ch].enabled = true; }
void xdmac_channel_disable(Xdmac *, uint32_t ch) noexcept { hw.channels[ch].enabled = false; }

uint32_t xdmac_channel_get_status(Xdmac *) noexcept
{
	uint32_t status = 0;
	for (size_t ch = 0; ch < ARRAY_SIZE(hw.channels); ++ch)
	{
		if (hw.channels[ch].enabled) { status |= 1u << ch; }
	}
	return status;
}

void NVIC_SetPriority(IRQn, uint32_t) noexcept { }
void NVIC_EnableIRQ(IRQn irq) noexcept { if (irq == SBC_SPI_IRQn) { hw.nvicEnabled = true; } }

void digitalWrite(Pin p, bool high) noexcept { if (p == SbcTfrReadyPin) { hw.tfrRdy = high; } }
void SetPinMode(Pin p, PinMode m) noexcept { if (p == SbcTfrReadyPin) { hw.tfrRdy = (m == OUTPUT_HIGH); } }

void InitAllocationFromPbufPool() noexcept
{
	memset(pbufArena, 0, sizeof(pbufArena));
	pbufUsed = 0;
}

void *AllocateFromPbufPool(size_t bytes) noexcept
{
	bytes = (bytes + 15) & ~(size_t)15;
	if (pbufUsed + bytes > sizeof(pbufArena))
	{
		return nullptr;
	}
	void *const p = pbufArena + pbufUsed;
	pbufUsed += bytes;
	return p;
}

// One SPI exchange framed by NSS. Returns true if the NSS rising interrupt ran.
static bool Exchange(const std::vector<uint8_t>& mosi, std::vector<uint8_t>& miso) noexcept
{
	const XdmacChannel& rx = hw.channels[DmacChanSbcRx];
	const XdmacChannel& tx = hw.channels[DmacChanSbcTx];
	const bool rxActive = hw.spiEnabled && rx.enabled;
	const bool txActive = hw.spiEnabled && tx.enabled;
	const size_t len = mosi.size();

	// RRF receives min(len, DMA length) bytes; the rest of its target keeps its content
	const size_t received = rxActive ? std::min<size_t>(len, rx.ubc) : 0;
	if (received != 0)
	{
		memcpy(AddressToPointer(rx.da), mosi.data(), received);
	}

	// DSF reads RRF's tx bytes for the DMA length and the underrun byte beyond
	miso.resize(len);
	for (size_t i = 0; i < len; ++i)
	{
		miso[i] = (txActive && i < tx.ubc) ? AddressToPointer(tx.sa)[i] : underrunByte;
	}

	if (hw.spiEnabled && (hw.spiImr & SPI_IER_NSSR) != 0 && hw.nvicEnabled)
	{
		uint32_t status = SPI_SR_NSSR;
		if (rxActive && len >= rx.ubc + 2) { status |= SPI_SR_OVRES; }		// only feeds the M122 counters
		if (txActive && len > tx.ubc) { status |= SPI_SR_UNDES; }
		HarnessSpi.SPI_SR.value |= status;
		SBC_SPI_HANDLER();
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Replay

void HarnessInternalError(const char *file, int line) noexcept
{
	++counters.internalErrors;
	fprintf(stderr, "internal error at %s:%d\n", file, line);
}

int debugPrintf(const char *fmt, ...) noexcept
{
	if (!debugOutput) { return 0; }
	va_list vargs;
	va_start(vargs, fmt);
	fputs("rrf: ", stderr);
	const int ret = vfprintf(stderr, fmt, vargs);
	va_end(vargs);
	return ret;
}

bool RepRap::Debug(Module) const noexcept { return debugOutput; }

void RepRap::EmergencyStop() noexcept
{
	++counters.emergencyStops;
	if (traceOutput) { fprintf(stderr, "trace: EmergencyStop\n"); }
}

void Platform::Message(MessageType, const char *msg) noexcept
{
	if (traceOutput) { fprintf(stderr, "msg: %s", msg); }
}

void Platform::MessageF(MessageType, const char *fmt, ...) noexcept
{
	if (!traceOutput) { return; }
	va_list vargs;
	va_start(vargs, fmt);
	fputs("msg: ", stderr);
	vfprintf(stderr, fmt, vargs);
	va_end(vargs);
}

void Platform::ReinitUsbDevice(unsigned int) noexcept { }
void Platform::DisconnectUsb() noexcept { }

size_t StringRef::copy(const char *s, size_t n) const noexcept
{
	const size_t l = std::min(n, len - 1);
	memcpy(p, s, l);
	p[l] = 0;
	return l;
}
int StringRef::lcat(const char *) const noexcept { return 0; }
int StringRef::lcatf(const char *, ...) const noexcept { return 0; }
int StringRef::printf(const char *, ...) const noexcept { return 0; }

namespace
{
	bool instanceActive = false;

	void NewInstance(unsigned int skip, uint8_t underrun) noexcept
	{
		// Boot state of DataTransfer's static memory: non-cached RAM is zeroed by the startup code
		memset((void *)&DataTransfer::rxHeader, 0, sizeof(DataTransfer::rxHeader));
		memset((void *)&DataTransfer::txHeader, 0, sizeof(DataTransfer::txHeader));
		memset((void *)&DataTransfer::rxResponse, 0, sizeof(DataTransfer::rxResponse));
		memset((void *)&DataTransfer::txResponse, 0, sizeof(DataTransfer::txResponse));
#if SUPPORTS_SBC_OVER_USB
		memset((void *)&DataTransfer::usbRxHeader, 0, sizeof(DataTransfer::usbRxHeader));
		memset((void *)&DataTransfer::usbTxHeader, 0, sizeof(DataTransfer::usbTxHeader));
#endif
		dataReceived = false;
		transferReadyHigh = false;
		spiTxUnderruns = 0;
		spiRxOverruns = 0;
		memset(&hw, 0, sizeof(hw));
		HarnessSpi.SPI_SR.value = 0;
		counters = Counters();
		notifyCount = 0;
		inDelay = false;
		virtualTime = 0;
		got.clear();
		skipTransfer = skip;
		underrunByte = underrun;

		if (sbc != nullptr)
		{
			sbc->~SbcInterface();				// the old TaskLoop coroutine is simply abandoned
		}
		sbc = new (sbcStorage) SbcInterface();
		sbc->transfer.Init();					// SbcInterface::Init
		WriteHarnessPacket(1);

		getcontext(&taskContext);
		taskContext.uc_stack.ss_sp = taskStack;
		taskContext.uc_stack.ss_size = sizeof(taskStack);
		taskContext.uc_link = nullptr;
		makecontext(&taskContext, TaskEntry, 0);
		SwitchToTask();							// InitFromTask, StartNextTransfer, DoTransfer, until the first wait
		instanceActive = true;
	}

	const char *WaitName(uint32_t timeout) noexcept
	{
		static char buf[24];
		if (timeout == SbcConnectionTimeout) { return "full"; }
		if (timeout == SbcTransferTimeout) { return "partial"; }
		if (timeout == TaskBase::TimeoutUnlimited) { return "unlimited"; }
		snprintf(buf, sizeof(buf), "%" PRIu32 "ms", timeout);
		return buf;
	}

	std::string Status() noexcept
	{
		const unsigned int s = (unsigned int)sbc->transfer.state;
		std::string gotList;
		for (const std::string& g : got)
		{
			if (!gotList.empty()) { gotList += ','; }
			gotList += g;
		}
		char buf[256];
		snprintf(buf, sizeof(buf), "state=%s pin=%d conn=%d disc=%u resets=%u wait=%s got=%s",
				 (s < ARRAY_SIZE(StateNames)) ? StateNames[s] : "?", (int)hw.tfrRdy, (int)sbc->isConnected,
				 counters.emergencyStops, counters.resets, inDelay ? "delay" : WaitName(blockedTimeout), gotList.empty() ? "-" : gotList.c_str());
		return buf;
	}

	bool ParseHex(const std::string& s, std::vector<uint8_t>& out) noexcept
	{
		out.clear();
		if (s == "-") { return true; }
		if (s.size() % 2 != 0) { return false; }
		for (size_t i = 0; i < s.size(); i += 2)
		{
			if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) { return false; }
			out.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
		}
		return true;
	}

	std::string ToHex(const std::vector<uint8_t>& bytes) noexcept
	{
		if (bytes.empty()) { return "-"; }
		static const char digits[] = "0123456789abcdef";
		std::string s;
		for (uint8_t b : bytes) { s += digits[b >> 4]; s += digits[b & 15]; }
		return s;
	}

	void CheckLowAddress(const void *p, const char *what) noexcept
	{
		if ((uintptr_t)p > 0xFFFFFFFFu)
		{
			fprintf(stderr, "%s is above 4 GB, build with -no-pie\n", what);
			exit(2);
		}
	}
}

int main(int argc, char **argv)
{
	bool ackNew = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "-d") { debugOutput = true; }
		else if (a == "-t") { traceOutput = true; }
		else if (a == "-n") { ackNew = true; }
		else if (a == "-b") { delayBlocks = true; }
		else
		{
			fprintf(stderr, "usage: %s [-d] [-t] [-n] [-b]\n  -d  RRF debug output to stderr\n  -t  harness trace to stderr\n  -n  print an N status line after NEW\n  -b  delay() blocks until T\n", argv[0]);
			return 2;
		}
	}

	CheckLowAddress(&DataTransfer::rxHeader, "DataTransfer::rxHeader");
	CheckLowAddress(pbufArena + sizeof(pbufArena), "the PBUF arena");
	CheckLowAddress(&HarnessSpi + 1, "the SPI registers");

	std::string line;
	while (std::getline(std::cin, line))
	{
		std::vector<std::string> tok;
		for (size_t pos = 0; pos < line.size(); )
		{
			while (pos < line.size() && isspace((unsigned char)line[pos])) { ++pos; }
			const size_t start = pos;
			while (pos < line.size() && !isspace((unsigned char)line[pos])) { ++pos; }
			if (pos > start) { tok.push_back(line.substr(start, pos - start)); }
		}
		if (tok.empty() || tok[0][0] == '#')
		{
			continue;
		}

		const std::string& cmd = tok[0];
		if (cmd == "NEW")
		{
			std::vector<uint8_t> u;
			if (tok.size() != 3 || !isdigit((unsigned char)tok[1][0]) || !ParseHex(tok[2].substr(tok[2].rfind('x') + 1), u) || u.size() != 1)
			{
				printf("E bad NEW\n");
			}
			else
			{
				NewInstance((unsigned int)strtoul(tok[1].c_str(), nullptr, 10), u[0]);
				if (ackNew) { printf("N %s\n", Status().c_str()); }
			}
		}
		else if (cmd == "X")
		{
			std::vector<uint8_t> mosi, miso;
			if (!instanceActive) { printf("E no instance\n"); }
			else if (tok.size() > 2 || !ParseHex((tok.size() == 2) ? tok[1] : "-", mosi)) { printf("E bad X\n"); }
			else
			{
				got.clear();
				if (Exchange(mosi, miso) && !inDelay)
				{
					SwitchToTask();				// TakeIndexed returns true, TaskLoop runs until its next wait
				}
				printf("R %s %s\n", ToHex(miso).c_str(), Status().c_str());
			}
		}
		else if (cmd == "T")
		{
			if (!instanceActive) { printf("E no instance\n"); }
			else if (blockedTimeout == TaskBase::TimeoutUnlimited) { printf("E T during an unlimited wait\n"); }
			else
			{
				got.clear();
				SwitchToTask();					// TakeIndexed returns false, TaskLoop runs until its next wait
				printf("T %s\n", Status().c_str());
			}
		}
		else if (cmd == "END")
		{
			instanceActive = false;
		}
		else
		{
			printf("E unknown command %s\n", cmd.c_str());
		}
		fflush(stdout);
	}
	return 0;
}
