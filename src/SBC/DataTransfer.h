/*
 * DataTransfer.h
 *
 *  Created on: 29 Mar 2019
 *      Author: Christian
 */

#ifndef SRC_SBC_DATATRANSFER_H_
#define SRC_SBC_DATATRANSFER_H_

#include "RepRapFirmware.h"

#if HAS_SBC_INTERFACE

#include <GCodes/GCodeFileInfo.h>
#include <GCodes/GCodeChannel.h>
#include "SbcMessageFormats.h"
#include <RTOSIface/RTOSIface.h>

class BinaryGCodeBuffer;
class StringRef;
class OutputBuffer;
class GCodeMachineState;
class HeightMap;

struct ExpressionValue;

enum class TransferState
{
	doingFullTransfer,
	doingPartialTransfer,
	finishingTransfer,
	connectionTimeout,
	connectionReset,
	finished
};

// One-shot faults that M122 P1010..P1016 can arm, to exercise the SPI and code buffer recovery paths that are
// otherwise only reachable with a broken SBC, a noisy SPI link or a corrupted code buffer. Each of them is consumed
// by the first exchange that can use it and then disarms itself, so the link always recovers within one transfer.
enum class SbcFaultInjection : uint8_t
{
	none = 0,
	badTxHeaderChecksum,			// send the transfer header with a broken checksum, so that the SBC asks for it again
	badTxDataChecksum,				// send the transfer data with a broken checksum, so that the SBC asks for it again
	badRxHeaderChecksum,			// act as if the header we received had a bad checksum, so that we ask for it again
	badRxDataChecksum,				// act as if the data we received had a bad checksum, so that we ask for it again
	refuseNextCode,					// refuse the next code the way an over-long one is refused, so that the SBC resends it
	simulateTimeout					// act as if the connection had timed out, to exercise the disconnect and recovery path
};

// Check whether a packet header at the given offset, and the payload it declares, both lie inside a transfer of the
// given length. ReadPacket used to test only that the offset was below the transfer length, so a header straddling
// the end was read anyway, and ReadData never checked at all - see the call site for what that opened up.
// Note that the payload is deliberately measured unpadded. Padding is applied lazily by WritePacketHeader, at the
// start of the following packet, so the transfer length is the unpadded end of the last packet: rounding up here
// would refuse every transfer whose last packet does not happen to carry a whole number of dwords.
static inline constexpr bool PacketFitsInTransfer(size_t offset, uint16_t payloadLength, uint16_t transferLength) noexcept
{
	return offset + sizeof(PacketHeader) + payloadLength <= transferLength;
}

class DataTransfer
{
public:
	DataTransfer() noexcept;
	void Init() noexcept;
	void InitFromTask() noexcept;
	void Diagnostics(const StringRef& reply) noexcept;

	TransferState DoTransfer() noexcept;													// Try to finish the current transfer
	void StartNextTransfer() noexcept;														// Kick off the next transfer
	void ResetConnection(bool fullReset) noexcept;											// Reset the connection after a longer timeout

	size_t PacketsToRead() const noexcept;
	const PacketHeader *ReadPacket() noexcept;												// Attempt to read the next packet header or return null. Advances the read pointer to the next packet or the packet's data
	const char *ReadData(size_t packetLength) noexcept;										// Read the packet data and advance to the next packet (if any)
	bool ReadBoolean() noexcept;															// Read a boolean value
	void ReadGetObjectModel(size_t packetLength, const StringRef &key, const StringRef &flags) noexcept;		// Read an object model request
	void ReadPrintStartedInfo(size_t packetLength, const StringRef& filename, GCodeFileInfo &info) noexcept;	// Read info about the started file print
	PrintStoppedReason ReadPrintStoppedInfo() noexcept;										// Read info about why the print has been stopped
	GCodeChannel ReadMacroCompleteInfo(bool &error) noexcept;								// Read info about a completed macro file
	GCodeChannel ReadCodeChannel() noexcept;												// Read a code channel
	GCodeChannel ReadEvaluateExpression(size_t packetLength, const StringRef& expression) noexcept;	// Read an expression request
	bool ReadMessage(MessageType& type, OutputBuffer *buf) noexcept;						// Read a request to output a message
	GCodeChannel ReadSetVariable(bool& createVariable, const StringRef& varName, const StringRef& expression) noexcept;	// Read a variable set request
	GCodeChannel ReadDeleteLocalVariable(const StringRef& varName) noexcept;				// Read a variable deletion request
	FileHandle ReadOpenFileResult(FilePosition& fileLength) noexcept;						// Read the result of a file open request
	int ReadFileData(char *buffer, size_t length) noexcept;									// Read file data from the SBC

	void InjectFault(SbcFaultInjection fault) noexcept { pendingFault = fault; }		// Arm a one-shot fault, see M122 P1010..P1016
	bool TakeInjectedFault(SbcFaultInjection fault) noexcept;						// Test for the given armed fault and disarm it
	SbcFaultInjection GetInjectedFault() const noexcept { return pendingFault; }

	void ResendPacket(const PacketHeader *packet) noexcept;
	bool WriteObjectModel(OutputBuffer *data) noexcept;
	bool WriteCodeBufferUpdate(uint16_t bufferSpace) noexcept;
	bool WriteCodeReply(MessageType type, OutputBuffer *&response) noexcept;
	bool WriteMacroRequest(GCodeChannel channel, const char *filename, bool fromCode) noexcept;
	bool WriteAbortFileRequest(GCodeChannel channel, bool abortAll) noexcept;
	bool WriteMacroFileClosed(GCodeChannel channel) noexcept;
	bool WritePrintPaused(FilePosition position, PrintPausedReason reason) noexcept;
	bool WriteLocked(GCodeChannel channel) noexcept;
	bool WriteEvaluationResult(const char *expression, const ExpressionValue& value) noexcept;
	bool WriteEvaluationResult(const char *expression, OutputBuffer *json) noexcept;
	bool WriteEvaluationError(const char *expression, const char *errorMessage) noexcept;
	bool WriteDoCode(GCodeChannel channel, const char *code, size_t length) noexcept;
	bool WriteWaitForAcknowledgement(GCodeChannel channel) noexcept;
	bool WriteMessageAcknowledged(GCodeChannel channel) noexcept;
	bool WriteSetVariableResult(const char *varName, const ExpressionValue& value) noexcept;
	bool WriteSetVariableResult(const char *varName, OutputBuffer *json) noexcept;
	bool WriteSetVariableError(const char *varName, const char *errorMessage) noexcept;
	bool WriteCheckFileExists(const char *filename) noexcept;
	bool WriteDeleteFileOrDirectory(const char *filename, bool recursive = false) noexcept;
	bool WriteOpenFile(const char *filename, bool forWriting, bool append, uint32_t preAllocSize) noexcept;
	bool WriteReadFile(FileHandle handle, size_t bufferSize) noexcept;
	bool WriteFileData(FileHandle handle, const char *data, size_t& length) noexcept;
	bool WriteSeekFile(FileHandle handle, FilePosition offset) noexcept;
	bool WriteTruncateFile(FileHandle handle) noexcept;
	bool WriteCloseFile(FileHandle handle) noexcept;

private:
	enum class InternalTransferState
	{
		ExchangingHeader,
		ExchangingHeaderResponse,
		ExchangingData,
		ExchangingDataResponse,
		ExchangingDataResponseRetry,
		ProcessingData,
		Resetting,
		ResettingDataResponse
	} state;

	// Transfer properties
	uint16_t lastTransferNumber;
	unsigned int failedTransfers, checksumErrors;

	// Fault injection. The two "corrupted" flags record that an injected corruption is still in the outgoing buffers
	// and has to be taken back out once it has been sent, because neither ExchangeHeader nor ExchangeData recomputes
	// a checksum when the SBC asks for a retry: a corruption left in place would be re-sent for as long as the SBC
	// keeps asking, and the link would only recover by timing out.
	volatile SbcFaultInjection pendingFault;
	bool txHeaderChecksumCorrupted, txDataCorrupted;

	// Transfer buffers
#if SAME70
	// SAME70 has a write-back cache, so these must be in non-cached memory because we DMA to/from them.
	// See http://ww1.microchip.com/downloads/en/DeviceDoc/Managing-Cache-Coherency-on-Cortex-M7-Based-MCUs-DS90003195A.pdf
	// This in turn means that we must declare them static, so we can only have one DataTransfer instance
	static __nocache TransferHeader rxHeader;
	static __nocache TransferHeader txHeader;
	static __nocache uint32_t rxResponse;
	static __nocache uint32_t txResponse;
#else
	// The other processors we support have write-through cache
	// Allocate the buffers in the object so that we can delete the object and recycle the memory if the SBC interface is not being used
	// Align the headers on 16-byte boundaries so that they span only one cache line
	alignas(16) TransferHeader rxHeader;
	alignas(16) TransferHeader txHeader;
	uint32_t rxResponse;
	uint32_t txResponse;
#endif
	char *rxBuffer;				// not allocated until we know we need it
	char *txBuffer;				// not allocated until we know we need it
	size_t rxPointer, txPointer;

	// Packet properties
	uint16_t packetId;

	bool IsConnectionReset() const noexcept;

	void ExchangeHeader() noexcept;
	void ExchangeResponse(uint32_t response) noexcept;
	void ExchangeData() noexcept;
	void RestartTransfer(bool ownRequest) noexcept;
	uint32_t CalcCRC32(const char *buffer, size_t length) const noexcept;

	template<typename T> const T *ReadDataHeader() noexcept;

	// Always keep enough tx space to allow resend requests in case RRF runs out of resources and cannot process an incoming request right away
	size_t FreeTxSpace() const noexcept { return SbcTransferBufferSize - AddPadding(txPointer) - rxHeader.numPackets * sizeof(PacketHeader); }

	bool CanWritePacket(size_t dataLength = 0) const noexcept;
	PacketHeader *WritePacketHeader(FirmwareRequest request, size_t dataLength = 0, uint16_t resendPacktId = 0) noexcept;
	void WriteData(const char *data, size_t length) noexcept;
	template<typename T> T *WriteDataHeader() noexcept;

	size_t AddPadding(size_t length) const noexcept;
};

// Note that this is deliberately not atomic: it is only ever armed from the Main task via M122 and only ever taken
// by the SBC task, and the worst a race can do is delay the injected fault by one transfer.
inline bool DataTransfer::TakeInjectedFault(SbcFaultInjection fault) noexcept
{
	if (pendingFault != fault)
	{
		return false;
	}
	pendingFault = SbcFaultInjection::none;
	return true;
}

inline bool DataTransfer::IsConnectionReset() const noexcept
{
	uint16_t nextTransferNumber = lastTransferNumber + 1u;
	return (rxHeader.formatCode == SbcFormatCode) && (rxHeader.sequenceNumber != nextTransferNumber);
}

inline size_t DataTransfer::PacketsToRead() const noexcept
{
	return rxHeader.numPackets;
}

inline void DataTransfer::ResendPacket(const PacketHeader *packet) noexcept
{
	WritePacketHeader(FirmwareRequest::ResendPacket, 0, packet->id);
}

inline bool DataTransfer::CanWritePacket(size_t dataLength) const noexcept
{
	return FreeTxSpace() >= sizeof(PacketHeader) + dataLength;
}

inline size_t DataTransfer::AddPadding(size_t length) const noexcept
{
	size_t extraBytes = (length & 3);
	return (extraBytes == 0) ? length : length + 4 - extraBytes;
}
#endif	// HAS_SBC_INTERFACE

#endif /* SRC_SBC_DATATRANSFER_H_ */
