// Host test of the room DataTransfer keeps in the transmit buffer for resend requests (FreeTxSpace and ResendPacket in
// src/SBC/DataTransfer.h), with the real DataTransfer.cpp and Storage/CRC32.cpp compiled unchanged against the shims of
// Tools/sbc-harness.
//
// SbcInterface::ExchangeData answers the packets of a transfer one by one and writes a ResendPacket request for each packet
// it cannot process (SbcInterface.cpp, end of the packet loop). ResendPacket writes its header without a check, because
// FreeTxSpace keeps 8 bytes per received packet free for it. Every other packet is written only if it fits into FreeTxSpace.
// The test sets the number of received packets, fills the buffer with a real EvaluationResult packet so that N bytes are
// left before the room for the resend requests, writes resend requests, then a code reply, an object model or file data as
// the SBC task does for the next packets and after the loop, then the resend requests of the remaining packets. After every
// step it checks that the buffer still holds a resend request for every packet that may still need one, and that FreeTxSpace
// reports no more than the space before that room. Each case runs in a child process, so that a write past the buffer, which
// AddressSanitizer reports in the ASan build, fails only that case.

#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <vector>

#include <HarnessShim.h>
#include <SBC/SbcMessageFormats.h>
#define private public				// the test reads txBuffer and txPointer and sets the transport and the number of received packets
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

	// What the SBC task writes after the first resend requests
	enum class Writer
	{
		codeReply,					// WriteQueuedCodeReplies after the packet loop: WriteCodeReply, which writes as much of the reply as fits
		objectModel,				// a later GetObjectModel packet in the loop: WriteObjectModel
		fileData,					// a file write after the loop (FileOperation::write): WriteFileData, which writes as much as fits
	};

	struct Case
	{
		bool usb;					// transport: USB reads the number of received packets from usbRxHeader, SPI from rxHeader (GetRxNumPackets)
		uint8_t rxPackets;			// packets received in this transfer, FreeTxSpace keeps 8 bytes per packet for a resend request
		size_t freeSpace;			// FreeTxSpace() after the fill
		uint8_t resendsBefore;		// resend requests written before the write, for the first packets
		Writer writer;
		size_t length;				// bytes of the code reply, object model or file data
	};

	const Case cases[] =
	{
		// Less than one packet header is left before the room for the resend requests, then a packet is not processed and
		// the SBC task writes on in the same transfer
		{ false, 2, 0, 1, Writer::codeReply, 1000 },
		{ false, 2, 4, 1, Writer::codeReply, 1000 },
		{ false, 1, 0, 1, Writer::codeReply, 1000 },		// the resend request fills the buffer to the last byte
		{ false, 3, 4, 1, Writer::objectModel, 100 },
		{ false, 2, 4, 1, Writer::fileData, 1000 },
		{ false, 4, 0, 4, Writer::codeReply, 1000 },		// no packet of the transfer is processed
		{ true, 2, 0, 1, Writer::codeReply, 1000 },
		// The resend requests that go into the room come after the last write of the transfer
		{ false, 2, 8, 1, Writer::codeReply, 1000 },		// the first resend request takes the last 8 bytes before the room, the second goes into it
		{ false, 4, 0, 0, Writer::codeReply, 1000 },		// nothing fits before the room, and all four resend requests still fit into it
		// Enough is left before the room
		{ false, 2, 200, 1, Writer::codeReply, 100 },		// the whole reply fits after the resend request
	};

	unsigned int failed = 0;

	const char *WriterName(Writer w) noexcept
	{
		switch (w)
		{
		case Writer::codeReply:		return "code reply";
		case Writer::objectModel:	return "object model";
		case Writer::fileData:		return "file data";
		}
		return "?";
	}

	// The smallest packet the writer writes, header included
	size_t MinPacketLength(const Case& c) noexcept
	{
		switch (c.writer)
		{
		case Writer::codeReply:		return sizeof(PacketHeader) + sizeof(MessageHeader) + min<size_t>(16, c.length);
		case Writer::objectModel:	return sizeof(PacketHeader) + sizeof(StringHeader) + c.length;
		case Writer::fileData:		return sizeof(PacketHeader) + sizeof(FileHandleHeader) + 1;
		}
		return 0;
	}

	// The writer must write its packet if it fits into the space left before the room for the resend requests after the resend
	// requests already written, which is the case whether those still count against that room or not
	bool MustFit(const Case& c) noexcept
	{
		return c.freeSpace >= c.resendsBefore * sizeof(PacketHeader) + MinPacketLength(c);
	}

	// Runs in the child. Prints the reasons of a failure and returns false
	bool RunCase(const Case& c) noexcept
	{
		bool ok = true;
		auto fail = [&ok](const std::string& reason) { printf("        %s\n", reason.c_str()); ok = false; };

		DataTransfer *const transfer = new DataTransfer;
		transfer->Init();
		if (c.usb)
		{
			// As after M576.1 (DataTransfer::SwitchToUsb), without a device: the Write functions do not use it
			transfer->transportType = SbcTransportType::usb;
			transfer->usbRxHeader.numPackets = c.rxPackets;
		}
		else
		{
			transfer->rxHeader.numPackets = c.rxPackets;
		}
		if (transfer->GetRxNumPackets() != c.rxPackets)
		{
			fail("setup: GetRxNumPackets is " + std::to_string(transfer->GetRxNumPackets()) + ", not " + std::to_string(c.rxPackets));
			return false;
		}

		// Fill the buffer with one EvaluationResult packet as an EvaluateExpression packet gets it (WriteEvaluationError),
		// which takes 8 + 8 + strlen(expression) + strlen(errorMessage) bytes
		const size_t end = SbcTransferBufferSize - c.rxPackets * sizeof(PacketHeader);		// where FreeTxSpace ends
		const size_t fill = end - c.freeSpace;
		if (fill != 0)
		{
			const std::string expression("e");
			const std::string error(fill - sizeof(PacketHeader) - sizeof(EvaluationResultHeader) - expression.size(), 'x');
			if (!transfer->WriteEvaluationError(GCodeChannel(1), expression.c_str(), error.c_str(), 0))
			{
				fail("setup: WriteEvaluationError did not fit");
				return false;
			}
		}
		if (transfer->FreeTxSpace() != c.freeSpace)
		{
			fail("setup: FreeTxSpace is " + std::to_string(transfer->FreeTxSpace()) + ", not " + std::to_string(c.freeSpace));
			return false;
		}

		// After every step: the buffer must still have room for a resend request of every packet that may still need one, and
		// FreeTxSpace must not report more than the space before that room. The case goes on after a FreeTxSpace that reports
		// too much, so that the next write shows what it leads to, and ends when the room is gone
		unsigned int resendsWritten = 0;
		auto checkRoom = [&](const char *step) noexcept -> bool
		{
			const long room = (long)SbcTransferBufferSize - (long)transfer->AddPadding(transfer->txPointer);
			const long stillNeeded = (long)(c.rxPackets - resendsWritten) * (long)sizeof(PacketHeader);
			const size_t free = transfer->FreeTxSpace();
			if (free > SbcTransferBufferSize || (long)free > room - stillNeeded)
			{
				fail(std::string(step) + ": FreeTxSpace is " + std::to_string(free) + ", but "
						+ std::to_string(max<long>(room - stillNeeded, 0)) + " bytes are free before the room for the resend requests");
			}
			if (room < stillNeeded)
			{
				fail(std::string(step) + ": txPointer is " + std::to_string(transfer->txPointer) + ", "
						+ ((room < 0) ? std::to_string(-room) + " bytes past the end of the buffer" : "only " + std::to_string(room) + " bytes are left")
						+ ", the resend requests of the " + std::to_string(c.rxPackets - resendsWritten) + " packets left need " + std::to_string(stillNeeded));
				return false;
			}
			return true;
		};

		auto resend = [&](uint16_t id) noexcept -> bool
		{
			const size_t packetStart = transfer->AddPadding(transfer->txPointer);
			PacketHeader packet{};
			packet.request = (uint16_t)SbcRequest::Code;
			packet.id = id;
			transfer->ResendPacket(&packet);
			++resendsWritten;
			const std::string step = "resend request " + std::to_string(resendsWritten);
			if (!checkRoom(step.c_str()))
			{
				return false;
			}
			const PacketHeader *const header = reinterpret_cast<const PacketHeader *>(transfer->txBuffer + packetStart);
			if (header->request != (uint16_t)FirmwareRequest::ResendPacket || header->length != 0 || header->resendPacketId != id)
			{
				fail(step + ": the ResendPacket packet is wrong");
				return false;
			}
			return true;
		};

		// The first packets are not processed
		for (unsigned int i = 0; i < c.resendsBefore; ++i)
		{
			if (!resend(100 + i))
			{
				return false;
			}
		}

		// The next packet or the code after the loop writes its packet
		std::vector<char> data(c.length);
		for (size_t i = 0; i < c.length; ++i)
		{
			data[i] = (char)('a' + i % 26);
		}
		const size_t packetStart = transfer->AddPadding(transfer->txPointer);
		bool written = false;
		size_t bytesWritten = 0;
		switch (c.writer)
		{
		case Writer::codeReply:
			{
				OutputBuffer buf(data.data(), c.length);
				OutputBuffer *response = &buf;
				written = transfer->WriteCodeReply(MessageType::UsbMessage, response);
				bytesWritten = c.length - buf.BytesLeft();
			}
			break;

		case Writer::objectModel:
			{
				OutputBuffer buf(data.data(), c.length);
				written = transfer->WriteObjectModel(&buf);
				bytesWritten = (written) ? c.length : 0;
			}
			break;

		case Writer::fileData:
			{
				size_t length = c.length;
				written = transfer->WriteFileData(TestHandle, data.data(), length);
				bytesWritten = c.length - length;
			}
			break;
		}
		if (!checkRoom(WriterName(c.writer)))
		{
			return false;
		}
		if (MustFit(c) && !written)
		{
			fail(std::string(WriterName(c.writer)) + ": not written, but " + std::to_string(MinPacketLength(c)) + " bytes fit");
		}
		if (written)
		{
			const PacketHeader *const header = reinterpret_cast<const PacketHeader *>(transfer->txBuffer + packetStart);
			const size_t dataStart = packetStart + sizeof(PacketHeader)
										+ ((c.writer == Writer::codeReply) ? sizeof(MessageHeader) : (c.writer == Writer::objectModel) ? sizeof(StringHeader) : sizeof(FileHandleHeader));
			if (bytesWritten == 0 || memcmp(transfer->txBuffer + dataStart, data.data(), bytesWritten) != 0
				|| header->length != dataStart - packetStart - sizeof(PacketHeader) + bytesWritten)
			{
				fail(std::string(WriterName(c.writer)) + ": the packet is wrong");
			}
		}

		// The remaining packets are not processed either
		while (resendsWritten < c.rxPackets)
		{
			if (!resend(200 + resendsWritten))
			{
				return false;
			}
		}
		return ok;
	}

	void Report(const Case& c) noexcept
	{
		char name[200];
		snprintf(name, sizeof(name), "%s, %u packets received, %3zu bytes free, %u resend request(s), then %s of %zu bytes%s",
					(c.usb) ? "USB" : "SPI", (unsigned int)c.rxPackets, c.freeSpace, (unsigned int)c.resendsBefore, WriterName(c.writer), c.length,
					(MustFit(c)) ? " (fits)" : "");

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
