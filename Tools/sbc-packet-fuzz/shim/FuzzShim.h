// Host stand-ins for what the real SBC/DataTransfer.cpp and the extracted text of SbcInterface.cpp need from RRF, CoreN2G and FreeRTOS.
// The string classes (StringRef, String<N>), NamedEnum, Bitmap, the length constants of src/Config/Configuration.h, TypeCode,
// GCodeChannel, GCodeFileInfo, MessageType, GCodeResult and the SBC message formats are the real ones. What is played here:
// - OutputBuffer: the class layout is a stand-in, OutputBuffer::cat(const char *, size_t) and copy(const char *, size_t) are the real text
//   of src/Platform/OutputMemory.cpp (gen/OutputBuffer.inc), with a pool of OUTPUT_BUFFER_COUNT buffers of OUTPUT_BUFFER_SIZE bytes;
// - the SPI hardware of the SAME70: register writes only, the fuzzer hands DataTransfer a received transfer directly (see README.md);
// - the SerialCDC device of the USB transport: it serves the bytes of the fuzz input, as DSF writes them;
// - the PBUF pool: every buffer is a separate heap block of exactly the requested size, so that AddressSanitizer sees a read or write
//   beyond one of DataTransfer's 8 KB buffers. On the board the three buffers are next to each other in the pool.

#ifndef SBC_PACKET_FUZZ_SHIM_H
#define SBC_PACKET_FUZZ_SHIM_H

#include <ecv_duet3d.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cinttypes>
#include <ctime>
#include <atomic>
#include <algorithm>
#include <new>

// Board configuration of the Duet 3 MB6HC release build (Pins_Duet3_MB6HC.h, RTOS build)
#define SAME70					1
#define SAME5x					0
#define SAM4E					0
#define SAM4S					0
#define SAMC21					0
#define RP2040					0
#define STM32					0
#ifndef DUET3
# define DUET3					1
#endif
#ifndef DUET3_MB6HC
# define DUET3_MB6HC			1
#endif
#define HAS_SBC_INTERFACE		1
#define SUPPORTS_SBC_OVER_SPI	1
#ifndef SUPPORTS_SBC_OVER_USB
# define SUPPORTS_SBC_OVER_USB	1			// CORE_USES_TINYUSB = RTOS && SAME70 in the MB6HC release
#endif
#define HAS_LWIP_NETWORKING		1
#define HAS_WIFI_NETWORKING		1
#define HAS_MASS_STORAGE		1
#define SUPPORT_CAN_EXPANSION	1
#define SUPPORT_ASYNC_MOVES		1			// Pins_Duet3_MB6HC.h

#define __nocache
#define null					_ecv_null
#define THROWS(...)				noexcept(false)
#define SPEED_CRITICAL
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))

template<class X> constexpr X min(X a, X b) noexcept { return (a < b) ? a : b; }
template<class X> constexpr X max(X a, X b) noexcept { return (a > b) ? a : b; }

#include <Config/Configuration.h>
#include <General/String.h>
#include <General/StringRef.h>
#include <General/StringFunctions.h>
#include <General/Bitmap.h>
#include <General/NamedEnum.h>
#include <GCodeResult.h>
#include <Platform/MessageType.h>
#include <ObjectModel/TypeCode.h>

void FuzzInternalError(const char *file, int line) noexcept;
#define REPORT_INTERNAL_ERROR	FuzzInternalError(__FILE__, __LINE__)

extern "C" void debugPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

// RepRapFirmware.h. On the board vAssertCalled resets the processor; here it ends the run with a report that the fuzz driver counts
[[noreturn]] void vAssertCalled(uint32_t line, const char *file) noexcept;
#define RRF_ASSERT(_expr) do { if (!(_expr)) { vAssertCalled(__LINE__, __FILE__); } } while (false)
uint32_t millis() noexcept;
void delay(uint32_t ms) noexcept;

// CoreN2G CoreIO.h
inline void memcpyu32(uint32_t *dst, const uint32_t *src, size_t numWords) noexcept
{
	while (numWords != 0)
	{
		*dst++ = *src++;
		--numWords;
	}
}
inline void memcpyf(float *dst, const float *src, size_t numFloats) noexcept
{
	memcpyu32(reinterpret_cast<uint32_t *>(dst), reinterpret_cast<const uint32_t *>(src), numFloats);
}

// RepRapFirmware.h
typedef uint32_t FileHandle;
const FileHandle noFileHandle = 0;
typedef uint32_t FilePosition;
const FilePosition noFilePosition = 0xFFFFFFFFu;
constexpr char HighestAxisLetter = 'z';
enum class Module : uint8_t { SbcInterface, Gcodes };

// Platform/OutputMemory.h. The members that the real cat and copy use have the names of the real class
class OutputBuffer
{
public:
	explicit OutputBuffer(OutputBuffer *n) noexcept : next(n) { }
	OutputBuffer *Next() const noexcept { return next; }
	const char *Data() const noexcept { return data; }
	const char *UnreadData() const noexcept { return data + bytesRead; }
	size_t DataLength() const noexcept { return dataLength; }
	size_t Length() const noexcept;
	void Taken(size_t len) noexcept { bytesRead += len; }
	size_t BytesLeft() const noexcept { return dataLength - bytesRead; }
	bool HadOverflow() const noexcept { return hadOverflow; }
	void Clear() noexcept;

	size_t copy(const char *src, size_t len) noexcept;
	size_t cat(const char *src, size_t len) noexcept;
	size_t cat(const char *src) noexcept { return cat(src, strlen(src)); }

	static bool Allocate(OutputBuffer *&buf, bool isAppending = false) noexcept;
	static OutputBuffer *Release(OutputBuffer *buf) noexcept;
	static void ReleaseAll(OutputBuffer *&buf) noexcept;
	static unsigned int GetFreeBuffers() noexcept;
	static void ResetPool() noexcept;

	OutputBuffer *next;
	OutputBuffer *last;
	std::atomic<size_t> references;
	size_t dataLength, bytesRead;
	bool isReferenced, hadOverflow;
	char data[OUTPUT_BUFFER_SIZE];
};

// ObjectModel: what DataTransfer's Write functions and the request handlers read of an expression value
struct FuzzStringPtr { const char *Ptr() const noexcept { return ""; } };
struct FuzzStringHandle
{
	size_t GetLength() const noexcept { return 0; }
	FuzzStringPtr Get() const noexcept { return FuzzStringPtr(); }
};
struct FuzzArrayHandle { };
class ObjectModel;

struct ExpressionValue
{
	uint8_t type = 0;
	uint32_t param = 0;
	union { bool bVal; char cVal; float fVal; int32_t iVal; uint32_t uVal; const char *sVal; const ObjectModel *omVal; };
	FuzzStringHandle shVal;
	FuzzArrayHandle ahVal;
	ExpressionValue() noexcept : uVal(0) { }
	TypeCode GetType() const noexcept { return (TypeCode)type; }
	void AppendAsString(const StringRef& s) const noexcept { s.copy("value"); }
	uint64_t Get56BitValue() const noexcept { return 0; }
};

// RRFLibraries RTOSIface.h
class TaskBase
{
public:
	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;
	static void GiveFromISR(TaskBase *h, uint32_t index) noexcept;
	static bool TakeIndexed(uint32_t index, uint32_t timeout = TimeoutUnlimited) noexcept;
	static TaskBase *GetCallerTaskHandle() noexcept;
};
typedef TaskBase *TaskHandle;
namespace NotifyIndices { constexpr uint32_t SbcInterface = 2; }

struct Mutex { };
class MutexLocker
{
public:
	MutexLocker(Mutex&, uint32_t = TaskBase::TimeoutUnlimited) noexcept;
	bool IsAcquired() const noexcept { return acquired; }
	explicit operator bool() const noexcept { return acquired; }
private:
	bool acquired;
};
class TaskCriticalSectionLocker { public: TaskCriticalSectionLocker() noexcept { } };
struct ReadWriteLock { };
class ReadLocker { public: explicit ReadLocker(ReadWriteLock&) noexcept { } };
class BinarySemaphore { public: bool Give() noexcept { ++gives; return true; } unsigned int gives = 0; };

// CoreN2G Cache.h
namespace Cache
{
	inline void FlushBeforeDMASend(const volatile void *, size_t) noexcept { }
	inline void InvalidateAfterDMAReceive(const volatile void *, size_t) noexcept { }
}

// LwipEthernet/AllocateFromPbufPool.h
void InitAllocationFromPbufPool() noexcept;
void *AllocateFromPbufPool(size_t bytes) noexcept;

// Pins and GPIO (Pins_Duet3_MB6HC.h, CoreN2G)
typedef uint8_t Pin;
enum PinMode { OUTPUT_LOW, OUTPUT_HIGH };
enum class GpioPinFunction : uint8_t { A, B, C, D };
constexpr Pin SbcTfrReadyPin = 100;
constexpr Pin APIN_SBC_SPI_MOSI = 101, APIN_SBC_SPI_MISO = 102, APIN_SBC_SPI_SCK = 103, APIN_SBC_SPI_SS0 = 104;
constexpr GpioPinFunction SBCPinPeriphMode = GpioPinFunction::C;
inline void digitalWrite(Pin, bool) noexcept { }
inline void SetPinMode(Pin, PinMode) noexcept { }
inline void SetPinFunction(Pin, GpioPinFunction) noexcept { }
inline void SetDriveStrength(Pin, unsigned int) noexcept { }

typedef int IRQn;
constexpr IRQn SBC_SPI_IRQn = 42;
constexpr uint32_t NvicPrioritySpi = 1;
inline void NVIC_SetPriority(IRQn, uint32_t) noexcept { }
inline void NVIC_EnableIRQ(IRQn) noexcept { }

// SAME70 SPI1 (CMSIS component/spi.h, ASF spi.h): register writes only
constexpr uint32_t SPI_SR_RDRF = 1u << 0, SPI_SR_TDRE = 1u << 1, SPI_SR_MODF = 1u << 2, SPI_SR_OVRES = 1u << 3;
constexpr uint32_t SPI_SR_NSSR = 1u << 8, SPI_SR_TXEMPTY = 1u << 9, SPI_SR_UNDES = 1u << 10;
constexpr uint32_t SPI_IER_NSSR = 1u << 8;
constexpr uint32_t SPI_CSR_BITS_8_BIT = 0;
struct Spi { uint32_t SPI_SR, SPI_IER, SPI_IDR, SPI_TDR, SPI_RDR; };
extern Spi FuzzSpi;
#define SBC_SPI					(&FuzzSpi)
#define SBC_SPI_HANDLER			SPI1_Handler
extern "C" void SPI1_Handler() noexcept;
inline uint32_t spi_get_pcs(uint32_t n) noexcept { return (~(1u << n)) & 0x0F; }
inline void spi_reset(Spi *) noexcept { }
inline void spi_enable(Spi *) noexcept { }
inline void spi_disable(Spi *) noexcept { }
inline void spi_set_slave_mode(Spi *) noexcept { }
inline void spi_disable_mode_fault_detect(Spi *) noexcept { }
inline void spi_set_peripheral_chip_select_value(Spi *, uint32_t) noexcept { }
inline void spi_set_clock_polarity(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_set_clock_phase(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_set_bits_per_transfer(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_enable_clock(Spi *) noexcept { }

// SAME70 XDMAC (CoreN2G xdmac.h, DmacManager.h): register writes only
typedef uint8_t DmaChannel;
constexpr DmaChannel DmacChanSbcTx = 5;
constexpr DmaChannel DmacChanSbcRx = 6;
enum class DmaTrigSource : uint32_t { spi1tx = 3, spi1rx = 4 };
struct xdmac_channel_config_t { uint32_t mbr_ubc, mbr_sa, mbr_da, mbr_cfg, mbr_bc, mbr_ds, mbr_sus, mbr_dus; };
struct Xdmac { uint32_t dummy; };
extern Xdmac FuzzXdmac;
#define XDMAC					(&FuzzXdmac)
#define XDMAC_CC_TYPE_PER_TRAN			0u
#define XDMAC_CC_MBSIZE_SINGLE			0u
#define XDMAC_CC_DSYNC_MEM2PER			0u
#define XDMAC_CC_DSYNC_PER2MEM			0u
#define XDMAC_CC_CSIZE_CHK_1			0u
#define XDMAC_CC_DWIDTH_BYTE			0u
#define XDMAC_CC_SIF_AHB_IF0			0u
#define XDMAC_CC_SIF_AHB_IF1			0u
#define XDMAC_CC_DIF_AHB_IF0			0u
#define XDMAC_CC_DIF_AHB_IF1			0u
#define XDMAC_CC_SAM_INCREMENTED_AM		0u
#define XDMAC_CC_SAM_FIXED_AM			0u
#define XDMAC_CC_DAM_INCREMENTED_AM		0u
#define XDMAC_CC_DAM_FIXED_AM			0u
#define XDMAC_CC_PERID(x)				((uint32_t)(x) << 24)
inline void xdmac_configure_transfer(Xdmac *, uint32_t, xdmac_channel_config_t *) noexcept { }
inline void xdmac_channel_enable(Xdmac *, uint32_t) noexcept { }
inline void xdmac_channel_disable(Xdmac *, uint32_t) noexcept { }
inline uint32_t xdmac_channel_get_status(Xdmac *) noexcept { return 0; }
inline void xdmac_channel_set_descriptor_control(Xdmac *, uint32_t, uint32_t) noexcept { }
inline void xdmac_disable_interrupt(Xdmac *, uint32_t) noexcept { }

// Platform and RepRap. The classes that only the extracted SbcInterface text uses are declared here and defined in sbc_packet_fuzz.cpp,
// so that both translation units see the same RepRap
class GCodes;
class PrintMonitor;
class VariableSet;
template<class T> class WriteLockedPointer;

// GCodes/GCodeException.h: what the request handlers do with one
class GCodeException
{
public:
	explicit GCodeException(const char *msg) noexcept : message(msg) { }
	void GetMessage(const StringRef& reply, const void *gb) const noexcept { (void)gb; reply.copy(message); }
private:
	const char *message;
};

// SbcInterface as DataTransfer.cpp sees it through reprap.GetSbcInterface() (SbcInterface.h is not included there, see the Makefile)
class FuzzSbcConnection
{
public:
	bool IsConnected() const noexcept;
};

class Platform
{
public:
	void Message(MessageType type, const char *msg) noexcept;
	void Message(MessageType type, OutputBuffer *buf) noexcept;
	void MessageF(MessageType type, const char *fmt, ...) noexcept __attribute__((format(printf, 3, 4)));
	void ReinitUsbDevice(unsigned int devIndex) noexcept;
	void DisconnectUsb() noexcept;
};

class RepRap
{
public:
	bool Debug(Module m) const noexcept;
	bool UsingSbcInterface() const noexcept { return true; }
	void EmergencyStop() noexcept;
	Platform& GetPlatform() noexcept { return platform; }
	FuzzSbcConnection& GetSbcInterface() noexcept { return sbcConnection; }
	GCodes& GetGCodes() noexcept;
	PrintMonitor& GetPrintMonitor() noexcept;
	OutputBuffer *GetModelResponse(const void *gb, const char *key, const char *flags) THROWS(GCodeException);
	Mutex& GetObjectModelReportMutex() noexcept { return objectModelReportMutex; }
	WriteLockedPointer<VariableSet> GetGlobalVariablesForWriting() noexcept;
	void PrepareToLoadIap() noexcept { }
	void StartIap(const char *) noexcept;
	void NetworkUpdated() noexcept { }
	void VolumesUpdated() noexcept { }
	void InputsUpdated() noexcept { }
	void GlobalUpdated() noexcept { }
private:
	Platform platform;
	FuzzSbcConnection sbcConnection;
	Mutex objectModelReportMutex;
};
extern RepRap reprap;

#if SUPPORTS_SBC_OVER_USB
// Devices.h: the USB device of the SBC link. DoTransferUsb reads DSF's header and data from it; what RRF writes is dropped
class SerialCDC
{
public:
	size_t readDirect(uint8_t *dst, size_t len, uint32_t timeout) noexcept;
	bool writeDirect(const uint8_t *src, size_t len, uint32_t timeout) noexcept;
	size_t write(const uint8_t *, size_t len) noexcept { return len; }
	void flush() noexcept { }
	void WaitForTxEmpty(uint32_t) noexcept { }
	void BeginDirectMode() noexcept { }
	void EndDirectMode() noexcept { }
	bool IsConnected() const noexcept { return true; }

	const uint8_t *data = nullptr;		// the bytes DSF writes
	size_t length = 0, position = 0;
};
#endif

#endif
