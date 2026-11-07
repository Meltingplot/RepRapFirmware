// Host stand-ins for everything the real SBC/DataTransfer.cpp needs from RRF, CoreN2G, RRFLibraries and FreeRTOS.
// Only the SPI hardware model (spi_*, xdmac_*, NVIC, TfrRdy pin) and the task notification are behaviour that the replay
// depends on; they are implemented in harness.cpp. Everything else here only has to compile, because the replay never
// reaches it (object model, expression values, file info, strings).

#ifndef HARNESS_SHIM_H
#define HARNESS_SHIM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cinttypes>
#include <ctime>
#include <atomic>
#include <algorithm>

// Board configuration of the Duet 3 MB6HC release build (Pins_Duet3_MB6HC.h without USE_EMBEDDED_FILES, RTOS build)
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
#define SUPPORT_CAN_EXPANSION	1
#define SUPPORT_BITMAP64		1			// ObjectModel/TypeCode.h: SAME70 || SAME5x

// Compiler and eCv annotations
#define __nocache
#define pre(...)
#define post(...)
#define _ecv_array
#define _ecv_from
#define _ecv_null
#define not_null(x)				(x)
#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))

void HarnessInternalError(const char *file, int line) noexcept;
#define REPORT_INTERNAL_ERROR	HarnessInternalError(__FILE__, __LINE__)

template<class X> constexpr X min(X a, X b) noexcept { return (a < b) ? a : b; }
template<class X> constexpr X max(X a, X b) noexcept { return (a > b) ? a : b; }

int debugPrintf(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
uint32_t millis() noexcept;
void delay(uint32_t ms) noexcept;
inline void memcpyf(float *dst, const float *src, size_t n) noexcept { memcpy(dst, src, n * sizeof(float)); }
inline void memcpyu32(uint32_t *dst, const uint32_t *src, size_t n) noexcept { memcpy(dst, src, n * sizeof(uint32_t)); }

#include <Platform/MessageType.h>			// real RRF header

// RepRapFirmware.h
typedef uint32_t FileHandle;
const FileHandle noFileHandle = 0;
typedef uint32_t FilePosition;
const FilePosition noFilePosition = 0xFFFFFFFFu;
constexpr unsigned int MaxFilaments = 20;
constexpr size_t StringLength50 = 50;
constexpr size_t MaxGCodeStringLength = 256;		// Config/Configuration.h

enum class Module : uint8_t { SbcInterface };

// CANlib GCodeResult.h (only the underlying type matters for SbcMessageFormats.h)
enum class GCodeResult : uint8_t { notFinished, ok, warning, error };

// RRFLibraries StringRef / String<N>: compile-only
class StringRef
{
public:
	StringRef(char *pp, size_t pl) noexcept : p(pp), len(pl) { }
	size_t copy(const char *s, size_t n) const noexcept;
	int lcat(const char *s) const noexcept;
	int lcatf(const char *fmt, ...) const noexcept;
	int printf(const char *fmt, ...) const noexcept;
	size_t strlen() const noexcept { return ::strlen(p); }
	const char *c_str() const noexcept { return p; }
private:
	char *p;
	size_t len;
};

template<size_t N> class String
{
public:
	String() noexcept { buf[0] = 0; }
	StringRef GetRef() noexcept { return StringRef(buf, N + 1); }
	size_t strlen() const noexcept { return ::strlen(buf); }
	const char *c_str() const noexcept { return buf; }
	int printf(const char *fmt, ...) noexcept __attribute__((format(printf, 2, 3)))
	{
		va_list vargs;
		va_start(vargs, fmt);
		const int ret = vsnprintf(buf, N + 1, fmt, vargs);
		va_end(vargs);
		return ret;
	}
	size_t copy(const char *s, size_t n) noexcept { return GetRef().copy(s, n); }
private:
	char buf[N + 1];
};

// Platform/OutputMemory.h: a single, unchained buffer is enough for the harness
class OutputBuffer
{
public:
	OutputBuffer(const char *d, size_t l) noexcept : data(d), len(l), bytesRead(0) { }
	size_t Length() const noexcept { return len; }
	size_t BytesLeft() const noexcept { return len - bytesRead; }
	const char *UnreadData() const noexcept { return data + bytesRead; }
	void Taken(size_t n) noexcept { bytesRead += n; }
	const char *Data() const noexcept { return data; }
	size_t DataLength() const noexcept { return len; }
	size_t copy(const char *, size_t n) noexcept { return n; }
	static OutputBuffer *Release(OutputBuffer *) noexcept { return nullptr; }
	static void ReleaseAll(OutputBuffer *&buf) noexcept { buf = nullptr; }
private:
	const char *data;
	size_t len, bytesRead;
};

// ObjectModel: compile-only
enum class TypeCode : uint8_t
{
	None, Bool_tc, DriverId_tc, Uint32, Int32, Char, Bitmap16, Bitmap32, Float, Uint64, Bitmap64, CString,
	IPAddress_tc, MacAddress_tc, DateTime_tc, Port_tc, UniqueId_tc, CanExpansionBoardDetails, HeapString
};

struct HarnessStringPtr { const char *Ptr() const noexcept { return ""; } };
struct HarnessStringHandle
{
	size_t GetLength() const noexcept { return 0; }
	HarnessStringPtr Get() const noexcept { return HarnessStringPtr(); }
};

struct ExpressionValue
{
	uint8_t type = 0;
	uint32_t param = 0;
	union { bool bVal; char cVal; float fVal; int32_t iVal; uint32_t uVal; const char *sVal; };
	HarnessStringHandle shVal;
	ExpressionValue() noexcept : uVal(0) { }
	TypeCode GetType() const noexcept { return (TypeCode)type; }
	void AppendAsString(const StringRef&) const noexcept { }
	uint64_t Get56BitValue() const noexcept { return 0; }
};

// GCodes/GCodeChannel.h
class GCodeChannel
{
public:
	explicit GCodeChannel(uint8_t pv) noexcept : v(pv) { }
	uint8_t RawValue() const noexcept { return v; }
	uint8_t ToBaseType() const noexcept { return v; }
	bool IsValid() const noexcept { return v < NumValues; }
	static constexpr uint8_t NumValues = 15;
private:
	uint8_t v;
};

// GCodes/GCodeFileInfo.h: compile-only
struct GCodeFileInfo
{
	void Init() noexcept { }
	FilePosition fileSize;
	time_t lastModifiedTime;
	float layerHeight;
	unsigned int numLayers;
	float objectHeight;
	float filamentNeeded[MaxFilaments];
	uint32_t printTime;
	uint32_t simulatedTime;
	unsigned int numFilaments;
	bool isValid;
	String<50> generatedBy;
};

// RRFLibraries RTOSIface.h: the notification is implemented by the harness
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

// CoreN2G Cache.h: the SAME70 buffers are in non-cached RAM, so nothing to model
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
void digitalWrite(Pin p, bool high) noexcept;
void SetPinMode(Pin p, PinMode m) noexcept;
inline void SetPinFunction(Pin, GpioPinFunction) noexcept { }
inline void SetDriveStrength(Pin, unsigned int) noexcept { }

typedef int IRQn;
constexpr IRQn SBC_SPI_IRQn = 42;
constexpr uint32_t NvicPrioritySpi = 1;
void NVIC_SetPriority(IRQn irq, uint32_t prio) noexcept;
void NVIC_EnableIRQ(IRQn irq) noexcept;

// SAME70 SPI1 (CMSIS component/spi.h, ASF spi.h)
constexpr uint32_t SPI_SR_RDRF = 1u << 0, SPI_SR_TDRE = 1u << 1, SPI_SR_MODF = 1u << 2, SPI_SR_OVRES = 1u << 3;
constexpr uint32_t SPI_SR_NSSR = 1u << 8, SPI_SR_TXEMPTY = 1u << 9, SPI_SR_UNDES = 1u << 10;
constexpr uint32_t SPI_IER_NSSR = 1u << 8;
constexpr uint32_t SPI_CSR_BITS_8_BIT = 0;

struct HarnessSpiSr
{
	uint32_t value;
	operator uint32_t() noexcept { const uint32_t v = value; value &= ~(SPI_SR_OVRES | SPI_SR_MODF | SPI_SR_NSSR | SPI_SR_UNDES); return v; }
};
struct HarnessSpiIer { void operator=(uint32_t v) noexcept; };
struct HarnessSpiIdr { void operator=(uint32_t v) noexcept; };

struct Spi
{
	HarnessSpiSr SPI_SR;
	HarnessSpiIer SPI_IER;
	HarnessSpiIdr SPI_IDR;
	uint32_t SPI_TDR;
	uint32_t SPI_RDR;
};
extern Spi HarnessSpi;
#define SBC_SPI					(&HarnessSpi)
#define SBC_SPI_HANDLER			SPI1_Handler
extern "C" void SPI1_Handler() noexcept;

inline uint32_t spi_get_pcs(uint32_t n) noexcept { return (~(1u << n)) & 0x0F; }
void spi_reset(Spi *p) noexcept;
void spi_enable(Spi *p) noexcept;
void spi_disable(Spi *p) noexcept;
inline void spi_set_slave_mode(Spi *) noexcept { }
inline void spi_disable_mode_fault_detect(Spi *) noexcept { }
inline void spi_set_peripheral_chip_select_value(Spi *, uint32_t) noexcept { }
inline void spi_set_clock_polarity(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_set_clock_phase(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_set_bits_per_transfer(Spi *, uint32_t, uint32_t) noexcept { }
inline void spi_enable_clock(Spi *) noexcept { }

// SAME70 XDMAC (CoreN2G xdmac.h, DmacManager.h)
typedef uint8_t DmaChannel;
constexpr DmaChannel DmacChanSbcTx = 5;			// Pins_Duet3_MB6HC.h
constexpr DmaChannel DmacChanSbcRx = 6;
enum class DmaTrigSource : uint32_t { spi1tx = 3, spi1rx = 4 };

struct xdmac_channel_config_t
{
	uint32_t mbr_ubc, mbr_sa, mbr_da, mbr_cfg, mbr_bc, mbr_ds, mbr_sus, mbr_dus;
};
struct Xdmac { uint32_t dummy; };
extern Xdmac HarnessXdmac;
#define XDMAC					(&HarnessXdmac)

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

void xdmac_configure_transfer(Xdmac *x, uint32_t ch, xdmac_channel_config_t *cfg) noexcept;
void xdmac_channel_enable(Xdmac *x, uint32_t ch) noexcept;
void xdmac_channel_disable(Xdmac *x, uint32_t ch) noexcept;
uint32_t xdmac_channel_get_status(Xdmac *x) noexcept;
inline void xdmac_channel_set_descriptor_control(Xdmac *, uint32_t, uint32_t) noexcept { }
inline void xdmac_disable_interrupt(Xdmac *, uint32_t) noexcept { }

// Platform and RepRap
class Platform
{
public:
	void Message(MessageType type, const char *msg) noexcept;
	void MessageF(MessageType type, const char *fmt, ...) noexcept __attribute__((format(printf, 3, 4)));
	void ReinitUsbDevice(unsigned int devIndex) noexcept;
	void DisconnectUsb() noexcept;
};

// SbcInterface as DataTransfer.cpp sees it through reprap.GetSbcInterface(): only the connection state. The members that
// TaskLoop uses are in harness.cpp
class HarnessSbcConnection
{
public:
	bool IsConnected() const noexcept;
};

class RepRap
{
public:
	bool Debug(Module m) const noexcept;
	bool UsingSbcInterface() const noexcept { return true; }
	void EmergencyStop() noexcept;
	Platform& GetPlatform() noexcept { return platform; }
	HarnessSbcConnection& GetSbcInterface() noexcept { return sbcConnection; }
private:
	Platform platform;
	HarnessSbcConnection sbcConnection;
};
extern RepRap reprap;

#if SUPPORTS_SBC_OVER_USB
// Devices.h: never used, the harness stays on SPI
class SerialCDC
{
public:
	size_t readDirect(uint8_t *, size_t, uint32_t) noexcept { return 0; }
	bool writeDirect(const uint8_t *, size_t, uint32_t) noexcept { return false; }
	size_t write(const uint8_t *, size_t len) noexcept { return len; }
	void flush() noexcept { }
	void WaitForTxEmpty(uint32_t) noexcept { }
	void BeginDirectMode() noexcept { }
	void EndDirectMode() noexcept { }
	bool IsConnected() const noexcept { return false; }
};
#endif

#endif
