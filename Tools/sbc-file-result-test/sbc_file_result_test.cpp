// Host test of the result of an SBC file operation that was in flight when the connection to the SBC ended. Compiled from the real text
// that the Makefile extracts:
// - main task: SbcInterface::FileExists, DeleteFileOrDirectory, SecureDeleteFile, GetFileList, OpenFile, ReadFile, WriteFile,
//   WriteFileAsync, WaitForAsyncWrite, SeekFile, TruncateFile, CloseFile and their helpers WaitForFileSlot, PostFileOperation and
//   DoFileOperation (src/SBC/SbcInterface.cpp); FileStore::Read, ReadLine, Seek and Position (src/Storage/FileStore.cpp)
// - SBC task: the part of SbcInterface::ExchangeData that sends a posted file operation, the cases of ExchangeData that handle the file
//   results, SbcInterface::FinishAsyncWrite and SbcInterface::InvalidateResources (src/SBC/SbcInterface.cpp), FileStore::Invalidate
//
// The main task (TaskPriority::SpinPriority) posts a file operation and waits on fileSemaphore until the SBC task (TaskPriority::SbcPriority)
// gives it, then reads the result from fileSuccess, fileOffset and fileHandle. The SBC task gives it when the result arrives, and also
// when the connection ends (TaskLoop or a new connection -> EndConnection -> InvalidateResources). The test plays the SBC task where the
// main task waits, and checks what each file operation returns when the connection ends while its request is in flight, after earlier
// operations that succeeded.

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#define _ecv_array
#define _ecv_null

#define HAS_SBC_INTERFACE		1
#define HAS_MASS_STORAGE		0		// so that the FileStore functions take their SBC path only
#define HAS_EMBEDDED_FILES		0

static std::vector<std::string> failures;
static void Check(bool ok, const std::string& what) noexcept
{
	if (!ok)
	{
		failures.push_back(what);
	}
}

#define REPORT_INTERNAL_ERROR		Check(false, std::string("internal error reported at ") + __FILE__ + ":" + std::to_string(__LINE__))

// As in src/RepRapFirmware.h and src/SBC/SbcMessageFormats.h
typedef uint32_t FileHandle;
const FileHandle noFileHandle = 0;
typedef uint32_t FilePosition;
constexpr uint32_t SbcMaxRequestTime = 3000;
constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFF;

// The task that runs the code under test. The main task blocks only where it takes a semaphore, so that is where the SBC task runs
enum class Task { main, sbc };
static Task currentTask = Task::main;
static void SbcTaskRuns() noexcept;

class BinarySemaphore
{
public:
	bool Take(uint32_t timeout = TimeoutUnlimited) noexcept
	{
		if (!given && timeout != 0)
		{
			SbcTaskRuns();
		}
		const bool wasGiven = given;
		given = false;
		return wasGiven;
	}
	bool Give() noexcept { given = true; return true; }

	bool given = false;
};

struct Mutex
{
	unsigned int holdCount = 0;
};

class MutexLocker
{
public:
	MutexLocker(Mutex& m, uint32_t = TimeoutUnlimited) noexcept : mutex(m) { ++mutex.holdCount; }
	~MutexLocker() noexcept { --mutex.holdCount; }

private:
	Mutex& mutex;
};

namespace NotifyIndices { constexpr uint32_t SbcInterface = 0; }
struct SbcTaskStub { void Give(uint32_t) noexcept { } };
static SbcTaskStub sbcTaskStub;
static SbcTaskStub *sbcTask = &sbcTaskStub;		// static Task<SBCTaskStackWords> *sbcTask in SbcInterface.cpp

enum MessageType { ErrorMessage };

class Platform
{
public:
	void Message(MessageType, const char *s) noexcept { messages.push_back(s); }
	void MessageF(MessageType, const char *fmt, ...) noexcept __attribute__((format(printf, 3, 4)))
	{
		char buf[200];
		va_list vargs;
		va_start(vargs, fmt);
		vsnprintf(buf, sizeof(buf), fmt, vargs);
		va_end(vargs);
		messages.push_back(buf);
	}

	std::vector<std::string> messages;
};

class Heat
{
public:
	void SwitchOffAll(bool) noexcept { }
};

// The channel aborts that InvalidateResources records are not part of this test, so there are no GBs
class GCodeChannel
{
public:
	explicit GCodeChannel(size_t v) noexcept : value(v) { }

private:
	size_t value;
};
constexpr size_t NumGCodeChannels = 15;

class GCodeBuffer
{
public:
	void ResetChannelEpoch(bool) noexcept { }
};

class GCodes
{
public:
	GCodeBuffer *GetGCodeBuffer(GCodeChannel) const noexcept { return nullptr; }
};

// As in src/Storage/FileStore.h
enum class OpenMode : uint8_t { read, write, writeWithCrc, append };
enum class FileUseMode : uint8_t { free, readOnly, readWrite, invalidated };

class FileWriteBuffer
{
public:
	char *Data() noexcept { return data; }
	size_t BytesStored() const noexcept { return stored; }

	char data[64];
	size_t stored = 0;
};

struct OutputStack
{
	void ReleaseAll() noexcept { }
};

// As in src/SBC/SbcMessageFormats.h, with the requests the test uses
struct PacketHeader
{
	uint16_t request;
	uint16_t id;
	uint16_t length;
	uint16_t resendPacketId;
};

enum class SbcRequest : uint16_t
{
	CheckFileExistsResult, FileDeleteResult, OpenFileResult, FileReadResult, FileWriteResult, FileSeekResult, FileTruncateResult, FileListResult
};

// What DSF replies to a file request
struct FileReply
{
	bool success = true;							// ReadBoolean; a failed read is -1 bytes
	FileHandle handle = noFileHandle;				// ReadOpenFileResult
	FilePosition fileSize = 0;
	std::string data;								// ReadFileData, ReadFileList
	bool endOfList = true;
};

// The transfer: every request fits, and the results come from the reply the test plays
class DataTransfer
{
public:
	bool WriteCheckFileExists(const char *) noexcept { return true; }
	bool WriteDeleteFileOrDirectory(const char *, bool = false) noexcept { return true; }
	bool WriteSecureDeleteFile(const char *) noexcept { return true; }
	bool WriteGetFileList(const char *, uint32_t, uint32_t) noexcept { return true; }
	bool WriteOpenFile(const char *, bool, bool, uint32_t) noexcept { return true; }
	bool WriteReadFile(FileHandle, size_t) noexcept { return true; }
	bool WriteFileData(FileHandle, const char *, size_t& length) noexcept { length = 0; return true; }
	bool WriteSeekFile(FileHandle, FilePosition) noexcept { return true; }
	bool WriteTruncateFile(FileHandle) noexcept { return true; }
	bool WriteCloseFile(FileHandle) noexcept { return true; }

	const char *ReadData(size_t) noexcept { return reply.data.c_str(); }
	bool ReadBoolean() noexcept { return reply.success; }
	FileHandle ReadOpenFileResult(FilePosition& fileLength) noexcept
	{
		fileLength = reply.fileSize;
		return reply.handle;
	}

	// Copies at most length bytes, as DataTransfer::ReadFileData does. The packet of the test holds exactly the reply data (see
	// Reply), so the cut at the packet end that DataTransfer::ReadFileData and ReadFileList also make is not played
	int ReadFileData(size_t packetLength, char *buffer, size_t length) noexcept
	{
		(void)packetLength;
		if (!reply.success)
		{
			return -1;
		}
		const size_t bytesToRead = std::min(reply.data.size(), length);
		memcpy(buffer, reply.data.data(), bytesToRead);
		return (int)bytesToRead;
	}

	size_t ReadFileList(size_t packetLength, char *buffer, size_t length, bool& endOfList) noexcept
	{
		(void)packetLength;
		endOfList = reply.endOfList;
		const size_t bytesToRead = std::min(reply.data.size(), length);
		memcpy(buffer, reply.data.data(), bytesToRead);
		return bytesToRead;
	}

	FileReply reply;
};

// The members of SbcInterface (src/SBC/SbcInterface.h) that the extracted functions use, public so that the test can play the SBC task
class SbcInterface
{
public:
	bool IsConnected() const noexcept { return isConnected; }

	bool FileExists(const char *filename) noexcept;
	bool DeleteFileOrDirectory(const char *fileOrDirectory, bool recursive = false) noexcept;
	bool SecureDeleteFile(const char *filename) noexcept;
	size_t GetFileList(const char *directory, uint32_t startIndex, char *buffer, size_t bufferLength, bool& endOfList) noexcept;
	FileHandle OpenFile(const char *filename, OpenMode mode, FilePosition& fileLength, uint32_t preAllocSize = 0) noexcept;
	int ReadFile(FileHandle handle, char *buffer, size_t bufferLength) noexcept;
	bool WriteFile(FileHandle handle, const char *buffer, size_t bufferLength) noexcept;
	bool WriteFileAsync(FileHandle handle, FileWriteBuffer *buffer, std::atomic<bool>& errorFlag) noexcept;
	void WaitForAsyncWrite() noexcept;
	bool SeekFile(FileHandle handle, FilePosition offset) noexcept;
	bool TruncateFile(FileHandle handle) noexcept;
	void CloseFile(FileHandle handle) noexcept;

	// SBC task
	void SendFileOperation() noexcept;							// the part of ExchangeData that sends a posted file operation
	void HandleFileResult(const PacketHeader *packet) noexcept;	// the cases of ExchangeData that handle a file result
	void InvalidateResources() noexcept;
	void RecordChannelAbort(GCodeBuffer&, bool) noexcept { }

	DataTransfer transfer;
	volatile bool isConnected = true;
	std::atomic<bool> delaying{false};
	bool printAborted = false;
	bool sbcPacketRead = true;
	volatile uint16_t rxPointer = 0, txPointer = 0, txEnd = 0;
	volatile bool sendBufferUpdate = false;

	Mutex fileMutex;
	unsigned int numOpenFiles = 0;
	BinarySemaphore fileSemaphore;
	BinarySemaphore asyncWriteSemaphore;

	enum class FileOperation {
		none,
		checkFileExists,
		deleteFileOrDirectory,
		deleteFileOrDirectoryRecursively,
		openRead,
		openWrite,
		openAppend,
		read,
		write,
		writeAsync,
		seek,
		truncate,
		close,
		secureDeleteFile,
		getFileList
	} fileOperation = FileOperation::none;
	std::atomic<bool> fileOperationPending{false};

	const char * filePath = nullptr;
	FileHandle fileHandle = noFileHandle;
	std::atomic<bool> fileSuccess{false};

	uint32_t filePreAllocSize = 0;
	char * fileReadBuffer = nullptr;
	const char * fileWriteBuffer = nullptr;
	size_t fileBufferLength = 0;
	FilePosition fileOffset = 0;
	bool fileListEndOfList = false;

	FileWriteBuffer *asyncWriteBuffer = nullptr;
	FileHandle asyncWriteHandle = noFileHandle;
	const char *asyncWriteData = nullptr;
	size_t asyncWriteLength = 0;
	std::atomic<bool> *asyncWriteErrorFlag = nullptr;

	OutputStack gcodeReply;
	Mutex gcodeReplyMutex;

	bool WaitForFileSlot() noexcept;
	void PostFileOperation(FileOperation f) noexcept;
	bool DoFileOperation(FileOperation f) noexcept;
	void FinishAsyncWrite(bool success) noexcept;
};

// The members of FileStore (src/Storage/FileStore.h) that the extracted functions use in SBC mode
class FileStore
{
public:
	int Read(char *_ecv_array buf, size_t nBytes) noexcept;
	int ReadLine(char *_ecv_array buf, size_t nBytes) noexcept;
	bool Seek(FilePosition pos) noexcept;
	FilePosition Position() const noexcept;
	void Invalidate() noexcept;
	bool IsOpen() const noexcept { return usageMode != FileUseMode::free && usageMode != FileUseMode::invalidated; }

	FileHandle handle = noFileHandle;
	FilePosition length = 0;
	FilePosition offset = 0;
	FileUseMode usageMode = FileUseMode::free;
};

class RepRap
{
public:
	Platform& GetPlatform() noexcept { return platform; }
	Heat& GetHeat() noexcept { return heat; }
	GCodes& GetGCodes() noexcept { return gcodes; }
	SbcInterface& GetSbcInterface() noexcept { return *sbc; }
	bool UsingSbcInterface() const noexcept { return true; }

	Platform platform;
	Heat heat;
	GCodes gcodes;
	SbcInterface *sbc = nullptr;
};
static RepRap reprap;

// The file that the test has open, which MassStorage::InvalidateAllFiles invalidates as it does every open file
static FileStore *openFile = nullptr;
static unsigned int writeBuffersReleased = 0;

namespace MassStorage
{
	void ReleaseWriteBuffer(FileWriteBuffer *) noexcept { ++writeBuffersReleased; }
	void InvalidateAllFiles() noexcept
	{
		if (openFile != nullptr && openFile->IsOpen())
		{
			openFile->Invalidate();
		}
	}
}

#include "SbcFileFunctions.inc"

void SbcInterface::SendFileOperation() noexcept
{
#include "SbcSendFileOperation.inc"
}

void SbcInterface::HandleFileResult(const PacketHeader *packet) noexcept
{
	switch ((SbcRequest)packet->request)
	{
#include "SbcFileResults.inc"

	default:
		break;
	}
}

#include "FileStore.inc"

// What the SBC task does while the main task waits for it, one step per wait
static std::deque<std::function<void()>> sbcSteps;
static SbcInterface *sbc = nullptr;

static void SbcTaskRuns() noexcept
{
	if (currentTask != Task::main)
	{
		Check(false, "the SBC task waited for itself");
		return;
	}
	if (sbcSteps.empty())
	{
		Check(false, "the main task waited for the SBC task, but no step of it was played");
		return;
	}
	const std::function<void()> step = sbcSteps.front();
	sbcSteps.pop_front();
	currentTask = Task::sbc;
	step();
	currentTask = Task::main;
}

// The SBC task sends the request in a transfer and DSF replies in a later one
static void Reply(SbcRequest request, const FileReply& reply) noexcept
{
	sbcSteps.push_back([request, reply]() {
		sbc->SendFileOperation();
		sbc->transfer.reply = reply;
		const PacketHeader packet = { (uint16_t)request, 0, (uint16_t)reply.data.size(), 0 };
		sbc->HandleFileResult(&packet);
	});
}

// The SBC task sends the request, then the connection is lost before the reply comes. TaskLoop clears isConnected and calls EndConnection,
// which calls InvalidateResources
static void LoseConnection() noexcept
{
	sbcSteps.push_back([]() {
		sbc->SendFileOperation();
		sbc->isConnected = false;
		sbc->InvalidateResources();
	});
}

// The SBC task sends the request, then the SBC starts a new connection before the reply comes, e.g. after a restart of DCS. ExchangeData
// sees the first marker and calls EndConnection, which calls InvalidateResources. The connection stays, because it carries the new one
static void NewConnection() noexcept
{
	sbcSteps.push_back([]() {
		sbc->SendFileOperation();
		sbc->InvalidateResources();
	});
}

// The SBC task sends the request, and no reply comes before the main task gives up after SbcMaxRequestTime
static void NoReply() noexcept
{
	sbcSteps.push_back([]() { sbc->SendFileOperation(); });
}

// The operations before the one under test: DSF opened the file and the last request was a seek, both successful. Their results stay in
// the members of SbcInterface. Returns the handle
constexpr FileHandle OpenedHandle = 7;
constexpr FilePosition OpenedSize = 20000;
constexpr FilePosition SeekPosition = 12345;

static FileHandle EarlierOperations() noexcept
{
	FileReply opened;
	opened.handle = OpenedHandle;
	opened.fileSize = OpenedSize;
	Reply(SbcRequest::OpenFileResult, opened);
	FilePosition length;
	const FileHandle handle = sbc->OpenFile("0:/sys/heightmap.csv", OpenMode::read, length);

	Reply(SbcRequest::FileSeekResult, FileReply());
	const bool sought = sbc->SeekFile(handle, SeekPosition);
	Check(handle == OpenedHandle && length == OpenedSize && sought, "the operations before the one under test failed");
	return handle;
}

// Open a file for reading as FileStore::Open does in SBC mode (src/Storage/FileStore.cpp, "handle = reprap.GetSbcInterface().OpenFile")
static void OpenForReading(FileStore& f, FilePosition size) noexcept
{
	FileReply opened;
	opened.handle = OpenedHandle;
	opened.fileSize = size;
	Reply(SbcRequest::OpenFileResult, opened);
	f.handle = sbc->OpenFile("0:/sys/heightmap.csv", OpenMode::read, f.length);
	f.offset = 0;
	f.usageMode = FileUseMode::readOnly;
	openFile = &f;
	Check(f.handle == OpenedHandle && f.length == size, "the open before the one under test failed");
}

// A line buffer and what follows it in memory, e.g. the rest of the stack frame of HeightMap::LoadFromFile. Stale bytes without line
// endings, as an unused stack is
constexpr size_t LineBufferSize = 16;
struct LineMemory
{
	LineMemory() noexcept { memset(bytes, 'x', sizeof(bytes)); }

	// The part behind the line buffer that ReadLine has changed, as a text for the report, or empty
	std::string Overrun() const noexcept
	{
		for (size_t i = LineBufferSize; i < sizeof(bytes); ++i)
		{
			if (bytes[i] != 'x')
			{
				return "ReadLine wrote behind its " + std::to_string(LineBufferSize) + "-byte buffer, at byte " + std::to_string(i);
			}
		}
		return std::string();
	}

	char bytes[1024];
};

static unsigned int passed = 0, failed = 0;

static void Report(const char *name, bool ok, const std::string& why) noexcept
{
	if (ok)
	{
		++passed;
		printf("PASS %s\n", name);
	}
	else
	{
		++failed;
		printf("FAIL %s: %s\n", name, why.c_str());
	}
}

// Run one case on a new SbcInterface, then check that the file operation slot is free for the next operation
static void Case(const char *name, const std::function<void()>& body) noexcept
{
	SbcInterface instance;
	sbc = &instance;
	reprap.sbc = &instance;
	openFile = nullptr;
	writeBuffersReleased = 0;
	sbcSteps.clear();
	failures.clear();
	currentTask = Task::main;

	body();

	Check(sbcSteps.empty(), "not every step played for the SBC task was used");
	Check(!instance.fileSemaphore.given, "fileSemaphore is left given, so the next file operation returns at once with this result");
	Check(instance.fileOperation == SbcInterface::FileOperation::none, "a file operation is still set");
	Check(!instance.fileOperationPending, "a file operation is still pending");
	Check(instance.fileMutex.holdCount == 0, "fileMutex is still held");

	std::string why;
	for (const std::string& f : failures)
	{
		why += (why.empty() ? "" : "; ") + f;
	}
	Report(name, why.empty(), why);
}

int main()
{
	// Each synchronous operation, while its request is in flight when the connection is lost, after an open and a seek that succeeded
	Case("FileExists returns false when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		Check(!sbc->FileExists("0:/sys/missing.g"), "returned true, the success of the seek before");
	});
	Case("DeleteFileOrDirectory returns false when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		Check(!sbc->DeleteFileOrDirectory("0:/gcodes/old.gcode"), "returned true, the success of the seek before");
	});
	Case("a recursive DeleteFileOrDirectory returns false when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		Check(!sbc->DeleteFileOrDirectory("0:/gcodes/old", true), "returned true, the success of the seek before");
	});
	Case("SecureDeleteFile returns false when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		Check(!sbc->SecureDeleteFile("0:/sys/secret.g"), "returned true, the success of the seek before");
	});
	Case("GetFileList returns no data when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		char buffer[64];
		bool endOfList = false;
		const size_t n = sbc->GetFileList("0:/gcodes", 0, buffer, sizeof(buffer), endOfList);
		Check(n == 0, "returned " + std::to_string(n) + " bytes of a listing that never arrived");
	});
	Case("OpenFile returns no handle when the connection is lost", []() {
		EarlierOperations();
		LoseConnection();
		FilePosition length = 1;
		const FileHandle handle = sbc->OpenFile("0:/sys/config-override.g", OpenMode::write, length);
		Check(handle == noFileHandle, "returned handle " + std::to_string(handle) + ", the handle of the file before");
		Check(length == 0, "returned length " + std::to_string(length));
	});
	Case("ReadFile returns -1 when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		LoseConnection();
		char buffer[LineBufferSize];
		const int n = sbc->ReadFile(handle, buffer, sizeof(buffer));
		Check(n == -1, "returned " + std::to_string(n) + " bytes read into a " + std::to_string(sizeof(buffer)) + "-byte buffer");
	});
	Case("ReadFile returns -1 when the SBC starts a new connection", []() {
		const FileHandle handle = EarlierOperations();
		NewConnection();
		char buffer[LineBufferSize];
		const int n = sbc->ReadFile(handle, buffer, sizeof(buffer));
		Check(n == -1, "returned " + std::to_string(n) + " bytes read into a " + std::to_string(sizeof(buffer)) + "-byte buffer");
	});
	Case("WriteFile returns false when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		LoseConnection();
		const char data[] = "M500 data";
		Check(!sbc->WriteFile(handle, data, sizeof(data) - 1), "returned true for data that was never written");
	});
	Case("SeekFile returns false when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		LoseConnection();
		Check(!sbc->SeekFile(handle, 100), "returned true, the success of the seek before");
	});
	Case("TruncateFile returns false when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		LoseConnection();
		Check(!sbc->TruncateFile(handle), "returned true, the success of the seek before");
	});
	Case("CloseFile returns when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		LoseConnection();
		sbc->CloseFile(handle);
	});

	// FileStore::ReadLine with a fixed buffer, as HeightMap::LoadFromFile (G29 S1) reads the height map
	Case("ReadLine of the first line after the open fails and stays in its buffer when the connection is lost", []() {
		FileStore f;
		OpenForReading(f, 600);
		LoseConnection();
		LineMemory memory;
		const int n = f.ReadLine(memory.bytes, LineBufferSize);
		Check(n == -1, "returned a line of " + std::to_string(n) + " characters");
		const std::string overrun = memory.Overrun();
		Check(overrun.empty(), overrun);
	});
	Case("ReadLine after lines longer than the buffer fails and stays in its buffer when the connection is lost", []() {
		FileStore f;
		OpenForReading(f, 600);
		LineMemory memory;
		FileReply line;
		line.data = std::string(40, 'a');
		Reply(SbcRequest::FileReadResult, line);
		Reply(SbcRequest::FileSeekResult, FileReply());
		const int first = f.ReadLine(memory.bytes, LineBufferSize);					// a split line: 15 characters, then a seek to 15
		line.data = std::string(40, 'b');
		Reply(SbcRequest::FileReadResult, line);
		Reply(SbcRequest::FileSeekResult, FileReply());
		const int second = f.ReadLine(memory.bytes, LineBufferSize);				// and a seek to 30
		Check(first == 15 && second == 15 && f.Position() == 30, "the reads before the one under test failed");

		LoseConnection();
		const int n = f.ReadLine(memory.bytes, LineBufferSize);
		Check(n == -1, "returned a line of " + std::to_string(n) + " characters");
		const std::string overrun = memory.Overrun();
		Check(overrun.empty(), overrun);
	});

	// The paths that already leave a failed result: a timeout of the wait, and a background write when the connection is lost
	Case("ReadFile returns -1 when the wait times out", []() {
		const FileHandle handle = EarlierOperations();
		NoReply();
		char buffer[LineBufferSize];
		const int n = sbc->ReadFile(handle, buffer, sizeof(buffer));
		Check(n == -1, "returned " + std::to_string(n));
	});
	Case("OpenFile returns no handle and length 0 when the wait times out", []() {
		EarlierOperations();
		NoReply();
		FilePosition length = 1;
		const FileHandle handle = sbc->OpenFile("0:/sys/config-override.g", OpenMode::write, length);
		Check(handle == noFileHandle && length == 0, "returned handle " + std::to_string(handle) + " and length " + std::to_string(length));
	});
	Case("a background write reports failure when the connection is lost", []() {
		const FileHandle handle = EarlierOperations();
		FileWriteBuffer buffer;
		buffer.stored = 10;
		std::atomic<bool> errorFlag{false};
		Check(sbc->WriteFileAsync(handle, &buffer, errorFlag), "the write was not posted");
		LoseConnection();
		sbc->WaitForAsyncWrite();
		Check(errorFlag, "the error flag of the file is not set");
		Check(writeBuffersReleased == 1, "the write buffer was not released");
	});

	printf("%u passed, %u failed\n", passed, failed);
	return (failed == 0) ? 0 : 1;
}
