// Host test of DataTransfer::WriteFileData (src/SBC/DataTransfer.cpp) at the end of the transmit buffer, with the real
// DataTransfer.cpp and Storage/CRC32.cpp compiled unchanged against the shims of Tools/sbc-harness.
//
// WriteFileData sends as much of a file write as fits into the current transfer and leaves the rest in `length` for the
// next one (SbcInterface.cpp, FileOperation::write and writeAsync). It must never write past the space FreeTxSpace reports,
// which is the transmit buffer minus the room kept for the resend requests of the packets received (DataTransfer.h).
// The test fills the transmit buffer with a real WriteCheckFileExists packet so that exactly N bytes are left, calls
// WriteFileData and checks the return value, the remaining length, the packet written and txPointer. Each case runs in
// a child process, so that a write past the buffer, which AddressSanitizer reports in the ASan build, fails only that case.

#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <vector>

#include <HarnessShim.h>
#include <SBC/SbcMessageFormats.h>
#define private public				// the test reads txBuffer and txPointer and sets the number of received packets
#include <SBC/DataTransfer.h>
#undef private

#ifdef __SANITIZE_ADDRESS__
// Exit code of a child that AddressSanitizer stops, told apart from a failed check (1). Leaks are not of interest here
extern "C" const char *__asan_default_options() { return "exitcode=99:detect_leaks=0"; }
#endif

// ---------------------------------------------------------------------------------------------------------------------
// What DataTransfer.cpp needs from the board (declared in ../sbc-harness/shim/HarnessShim.h). Nothing here is reached by
// the Write* functions except AllocateFromPbufPool, called by DataTransfer::Init

Spi HarnessSpi;
Xdmac HarnessXdmac;
RepRap reprap;

// Every buffer is a heap block of its own and exactly as long as requested, so AddressSanitizer reports the first byte
// written past it. On the board the buffers lie next to each other in the PBUF pool (DataTransfer::Init)
void InitAllocationFromPbufPool() noexcept { }
void *AllocateFromPbufPool(size_t bytes) noexcept { return malloc(bytes); }

void SetPinMode(Pin, PinMode) noexcept { }
void digitalWrite(Pin, bool) noexcept { }
void NVIC_SetPriority(IRQn, uint32_t) noexcept { }
void NVIC_EnableIRQ(IRQn) noexcept { }
void spi_reset(Spi *) noexcept { }
void spi_enable(Spi *) noexcept { }
void spi_disable(Spi *) noexcept { }
void HarnessSpiIer::operator=(uint32_t) noexcept { }
void HarnessSpiIdr::operator=(uint32_t) noexcept { }
void xdmac_configure_transfer(Xdmac *, uint32_t, xdmac_channel_config_t *) noexcept { }
void xdmac_channel_enable(Xdmac *, uint32_t) noexcept { }
void xdmac_channel_disable(Xdmac *, uint32_t) noexcept { }
uint32_t xdmac_channel_get_status(Xdmac *) noexcept { return 0; }
void TaskBase::GiveFromISR(TaskBase *, uint32_t) noexcept { }
TaskBase *TaskBase::GetCallerTaskHandle() noexcept { return nullptr; }
bool HarnessSbcConnection::IsConnected() const noexcept { return true; }
bool RepRap::Debug(Module) const noexcept { return false; }
int debugPrintf(const char *, ...) noexcept { return 0; }
void HarnessInternalError(const char *file, int line) noexcept { printf("        internal error at %s:%d\n", file, line); }
size_t StringRef::copy(const char *, size_t) const noexcept { return 0; }
int StringRef::lcat(const char *) const noexcept { return 0; }
int StringRef::lcatf(const char *, ...) const noexcept { return 0; }
int StringRef::printf(const char *, ...) const noexcept { return 0; }		// DataTransfer::GetSetVariableResultLength, not reached here

// ---------------------------------------------------------------------------------------------------------------------

namespace
{
	constexpr FileHandle TestHandle = 0x12345678;

	struct Case
	{
		uint8_t rxPackets;			// packets received in this transfer, FreeTxSpace keeps 8 bytes per packet for a resend request
		size_t freeSpace;			// FreeTxSpace() before WriteFileData
		size_t length;				// bytes to write
	};

	const Case cases[] =
	{
		// No packets received: FreeTxSpace ends at the end of the buffer
		{ 0, 0, 100 },
		{ 0, 4, 100 },
		{ 0, 8, 100 },				// the packet header fits, the file handle does not
		{ 0, 12, 100 },				// both headers fit, no data
		{ 0, 16, 100 },
		{ 0, 20, 100 },
		{ 0, 112, 100 },			// exactly the whole write
		{ 0, SbcTransferBufferSize, 100 },
		{ 0, SbcTransferBufferSize, 10000 },
		// Three packets received: FreeTxSpace ends 24 bytes before the end of the buffer
		{ 3, 8, 16 },				// a write past FreeTxSpace would stay inside the buffer, in the room kept for resend requests
		{ 3, 16, 100 },
	};

	unsigned int failed = 0;

	// The data bytes WriteFileData must write: as many as fit after the packet header and the file handle, and no packet at all
	// (return false) unless at least one byte fits
	size_t ExpectedBytes(const Case& c) noexcept
	{
		const size_t headers = sizeof(PacketHeader) + sizeof(FileHandleHeader);
		return (c.freeSpace > headers) ? min<size_t>(c.length, c.freeSpace - headers) : 0;
	}

	// Runs in the child. Prints the reasons of a failure and returns false
	bool RunCase(const Case& c) noexcept
	{
		bool ok = true;
		auto fail = [&ok](const std::string& reason) { printf("        %s\n", reason.c_str()); ok = false; };

		DataTransfer *const transfer = new DataTransfer;
		transfer->Init();
		DataTransfer::rxHeader.numPackets = c.rxPackets;
		const size_t reserved = c.rxPackets * sizeof(PacketHeader);
		const size_t end = SbcTransferBufferSize - reserved;		// where FreeTxSpace ends

		// Fill the buffer with one CheckFileExists packet, which takes 8 + 4 + strlen(filename) bytes
		const size_t fill = end - c.freeSpace;
		if (fill != 0)
		{
			const std::string filename(fill - sizeof(PacketHeader) - sizeof(StringHeader), 'f');
			if (!transfer->WriteCheckFileExists(filename.c_str()))
			{
				fail("setup: WriteCheckFileExists did not fit");
				return false;
			}
		}
		if (transfer->FreeTxSpace() != c.freeSpace)
		{
			fail("setup: FreeTxSpace is " + std::to_string(transfer->FreeTxSpace()) + ", not " + std::to_string(c.freeSpace));
			return false;
		}

		std::vector<char> data(c.length);
		for (size_t i = 0; i < c.length; ++i)
		{
			data[i] = (char)(i * 7 + 1);
		}

		const size_t txPointerBefore = transfer->txPointer;
		const uint16_t packetIdBefore = transfer->packetId;
		size_t length = c.length;
		const bool written = transfer->WriteFileData(TestHandle, data.data(), length);

		// Nothing may lie beyond the space FreeTxSpace reported, whether WriteFileData wrote a packet or not
		if (transfer->txPointer > end)
		{
			fail("txPointer is " + std::to_string(transfer->txPointer) + ", " + std::to_string(transfer->txPointer - end)
					+ " bytes past the end of the free space at " + std::to_string(end)
					+ ((transfer->txPointer > SbcTransferBufferSize) ? " and past the end of the buffer" : ""));
		}

		const size_t expectedBytes = ExpectedBytes(c);
		if (expectedBytes == 0)
		{
			// No data fits: WriteFileData must return false (SbcInterface tries again in the next transfer) and leave everything as it was
			if (written)
			{
				fail("returned true, but no data fits");
			}
			if (length != c.length)
			{
				fail("length is " + std::to_string(length) + ", not " + std::to_string(c.length));
			}
			if (transfer->txPointer != txPointerBefore || transfer->packetId != packetIdBefore)
			{
				fail("wrote a packet of " + std::to_string(transfer->txPointer - txPointerBefore) + " bytes");
			}
		}
		else
		{
			if (!written)
			{
				fail("returned false, but " + std::to_string(expectedBytes) + " bytes fit");
			}
			else
			{
				if (length != c.length - expectedBytes)
				{
					fail("length is " + std::to_string(length) + ", not " + std::to_string(c.length - expectedBytes));
				}
				const size_t packetStart = transfer->AddPadding(txPointerBefore);
				const PacketHeader *const header = reinterpret_cast<const PacketHeader *>(transfer->txBuffer + packetStart);
				const FileHandleHeader *const handle = reinterpret_cast<const FileHandleHeader *>(header + 1);
				const char *const payload = reinterpret_cast<const char *>(handle + 1);
				if (header->request != (uint16_t)FirmwareRequest::WriteFile || header->length != sizeof(FileHandleHeader) + expectedBytes
					|| handle->handle != TestHandle || memcmp(payload, data.data(), expectedBytes) != 0)
				{
					fail("the WriteFile packet is wrong");
				}
				const size_t expectedTxPointer = transfer->AddPadding(packetStart + sizeof(PacketHeader) + sizeof(FileHandleHeader) + expectedBytes);
				if (transfer->txPointer != expectedTxPointer)
				{
					fail("txPointer is " + std::to_string(transfer->txPointer) + ", not " + std::to_string(expectedTxPointer));
				}
			}
		}
		return ok;
	}

	void Report(const Case& c) noexcept
	{
		char name[160];
		snprintf(name, sizeof(name), "%3zu bytes free, %u packets received, write %zu bytes: %s", c.freeSpace, (unsigned int)c.rxPackets,
					c.length, (ExpectedBytes(c) == 0) ? "returns false and writes nothing" : "writes the bytes that fit");

		int fds[2];
		if (pipe(fds) != 0)
		{
			perror("pipe");
			exit(2);
		}
		fflush(stdout);
		const pid_t pid = fork();
		if (pid < 0)
		{
			perror("fork");
			exit(2);
		}
		if (pid == 0)
		{
			close(fds[0]);
			dup2(fds[1], STDOUT_FILENO);
			const bool ok = RunCase(c);
			fflush(stdout);
			_exit(ok ? 0 : 1);
		}

		close(fds[1]);
		std::string reasons;
		char buf[256];
		ssize_t n;
		while ((n = read(fds[0], buf, sizeof(buf))) > 0)
		{
			reasons.append(buf, n);
		}
		close(fds[0]);
		int status = 0;
		(void)waitpid(pid, &status, 0);

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			printf("PASS  %s\n", name);
			return;
		}
		++failed;
		printf("FAIL  %s\n%s", name, reasons.c_str());
		if (WIFEXITED(status) && WEXITSTATUS(status) == 99)
		{
			printf("        AddressSanitizer stopped the case, see its report above\n");
		}
		else if (WIFSIGNALED(status))
		{
			printf("        the case died with signal %d\n", WTERMSIG(status));
		}
		else if (!WIFEXITED(status) || WEXITSTATUS(status) != 1)
		{
			printf("        the case ended with status %d\n", status);
		}
	}
}

int main()
{
	for (const Case& c : cases)
	{
		Report(c);
	}
	printf("%u of %zu failed\n", failed, ARRAY_SIZE(cases));
	return (failed == 0) ? 0 : 1;
}
