// Host test of the ObjectModelKeyChanged request from the SBC. Compiled from the real text that the Makefile extracts: the case of
// SbcInterface::ExchangeData (src/SBC/SbcInterface.cpp), DataTransfer::ReadData, ReadDataHeader and ReadObjectModelKeyChanged
// (src/SBC/DataTransfer.cpp) and DataTransfer::AddPadding (src/SBC/DataTransfer.h).
//
// DSF sends the key as a string request, a StringHeader followed by the UTF-8 text (DSF Link/Adapter/SPI.cs WriteObjectModelKeyChanged,
// Link/Protocol/Writer.cs WriteStringRequest), and the case must increment the sequence number of "network" or "volumes". See README.md.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <General/String.h>
#include <General/SimpleMath.h>
#include <General/StringFunctions.h>

// As in src/Config/Configuration.h
constexpr size_t StringLength20 = 20;

// As in src/SBC/SbcMessageFormats.h
struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

struct StringHeader
{
	uint16_t length;
	uint16_t padding;
};

enum class SbcRequest : uint16_t
{
	ObjectModelKeyChanged = 30,
};

// As in src/Platform/RepRap.h, with the two sequence numbers that the case increments
class RepRap
{
public:
	void NetworkUpdated() noexcept { ++networkSeq; }
	void VolumesUpdated() noexcept { ++volumesSeq; }

	unsigned int networkSeq = 0, volumesSeq = 0;
};

static RepRap reprap;

// As in src/SBC/DataTransfer.h, with the members that the extracted functions use. The test puts each transfer in a heap block of exactly
// its size, so that AddressSanitizer sees a read beyond it
class DataTransfer
{
public:
	void Receive(const std::vector<char>& data) noexcept;
	const PacketHeader *ReadPacket() noexcept;				// as DataTransfer::ReadPacket, without its checks of the transfer
	size_t BytesRead() const noexcept { return rxPointer; }
	void Release() noexcept { delete[] rxBuffer; rxBuffer = nullptr; }

	const char *ReadData(size_t packetLength) noexcept;
	void ReadObjectModelKeyChanged(size_t packetLength, const StringRef& key) noexcept;

private:
	template<typename T> const T *ReadDataHeader() noexcept;
	size_t AddPadding(size_t length) const noexcept;

	char *rxBuffer = nullptr;
	size_t rxPointer = 0;
};

#include "AddPadding.inc"
#include "DataTransferRead.inc"

void DataTransfer::Receive(const std::vector<char>& data) noexcept
{
	delete[] rxBuffer;
	rxBuffer = new char[data.size()];
	memcpy(rxBuffer, data.data(), data.size());
	rxPointer = 0;
}

const PacketHeader *DataTransfer::ReadPacket() noexcept
{
	const PacketHeader *header = reinterpret_cast<const PacketHeader*>(rxBuffer + rxPointer);
	rxPointer += sizeof(PacketHeader);
	return header;
}

static DataTransfer transfer;

// The part of SbcInterface::ExchangeData that handles one packet
static void ProcessPacket(const PacketHeader *packet) noexcept
{
	switch ((SbcRequest)packet->request)
	{
#include "KeyChangedCase.inc"

	default:
		break;
	}
}

// Append a packet as DSF writes it: a PacketHeader with the length of the data (SPI.cs WriteObjectModelKeyChanged), then a StringHeader
// and the UTF-8 key, padded with zeros to a multiple of 4 (Writer.cs WriteStringRequest and AddPadding). A stringLength that is not
// negative replaces the length of the key in the StringHeader
static void AppendKeyPacket(std::vector<char>& data, const std::string& key, int stringLength = -1)
{
	PacketHeader packet = {};
	packet.request = (uint16_t)SbcRequest::ObjectModelKeyChanged;
	packet.length = (uint16_t)(sizeof(StringHeader) + key.size());
	StringHeader header = {};
	header.length = (uint16_t)((stringLength < 0) ? key.size() : (size_t)stringLength);

	const char *const p = reinterpret_cast<const char *>(&packet);
	data.insert(data.end(), p, p + sizeof(packet));
	const char *const h = reinterpret_cast<const char *>(&header);
	data.insert(data.end(), h, h + sizeof(header));
	data.insert(data.end(), key.begin(), key.end());
	while (data.size() % 4 != 0)
	{
		data.push_back(0);
	}
}

static unsigned int passed = 0, failed = 0;

// Process the packets of a transfer and check the sequence numbers and that the case read exactly the data of each packet
static void Check(const char *name, const std::vector<char>& data, unsigned int numPackets, unsigned int networkSeq, unsigned int volumesSeq) noexcept
{
	reprap = RepRap();
	transfer.Receive(data);
	for (unsigned int i = 0; i < numPackets; ++i)
	{
		ProcessPacket(transfer.ReadPacket());
	}

	std::string why;
	if (reprap.networkSeq != networkSeq) { why += " network sequence " + std::to_string(reprap.networkSeq) + " instead of " + std::to_string(networkSeq); }
	if (reprap.volumesSeq != volumesSeq) { why += " volumes sequence " + std::to_string(reprap.volumesSeq) + " instead of " + std::to_string(volumesSeq); }
	if (transfer.BytesRead() != data.size()) { why += " read " + std::to_string(transfer.BytesRead()) + " of " + std::to_string(data.size()) + " bytes"; }
	transfer.Release();

	if (why.empty())
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s:%s\n", name, why.c_str());
	}
}

int main()
{
	std::vector<char> data;

	// The keys that DSF sends (DSF Model/PeriodicUpdateService.cs)
	AppendKeyPacket(data, "network");
	Check("\"network\" increments the network sequence", data, 1, 1, 0);

	data.clear();
	AppendKeyPacket(data, "volumes");
	Check("\"volumes\" increments the volumes sequence", data, 1, 0, 1);

	data.clear();
	AppendKeyPacket(data, "network");
	AppendKeyPacket(data, "volumes");
	Check("\"network\" and \"volumes\" in one transfer increment both", data, 2, 1, 1);

	data.clear();
	AppendKeyPacket(data, "NETWORK");
	Check("the key is compared without case", data, 1, 1, 0);

	// Other keys
	data.clear();
	AppendKeyPacket(data, "move");							// a length that is a multiple of 4, so no zero follows the key in the packet
	Check("\"move\" changes no sequence", data, 1, 0, 0);

	data.clear();
	AppendKeyPacket(data, "networknetworknetwork");			// longer than String<StringLength20>
	Check("a key longer than 20 characters is not taken for \"network\"", data, 1, 0, 0);

	// A StringHeader length that does not match the packet
	data.clear();
	AppendKeyPacket(data, "network", 3);
	Check("the key is as long as the StringHeader says", data, 1, 0, 0);

	data.clear();
	AppendKeyPacket(data, "move", 200);						// at the end of the transfer, and no zero follows the key
	Check("a StringHeader length beyond the packet is not read beyond it", data, 1, 0, 0);

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
