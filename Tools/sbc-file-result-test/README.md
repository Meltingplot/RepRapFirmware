# SBC file result host test

This test runs the SBC file operations of `SbcInterface` from `src/SBC/SbcInterface.cpp` and `FileStore::Read` / `FileStore::ReadLine`
from `src/Storage/FileStore.cpp` on a PC. The Makefile copies their text out of the sources at build time with `extract.awk`, as
`Tools/kinematics-test` does with `Move::SetKinematics`, because the files as a whole need the hardware headers:
- main task: `FileExists`, `DeleteFileOrDirectory`, `SecureDeleteFile`, `GetFileList`, `OpenFile`, `ReadFile`, `WriteFile`,
  `WriteFileAsync`, `WaitForAsyncWrite`, `SeekFile`, `TruncateFile`, `CloseFile`, `WaitForFileSlot`, `PostFileOperation`, `DoFileOperation`;
  `FileStore::Read`, `ReadLine`, `Seek`, `Position`
- SBC task: the block of `ExchangeData` that sends a posted file operation (`if (fileOperationPending.load(...))`), the cases of
  `ExchangeData` that handle the file results, `FinishAsyncWrite`, `InvalidateResources`; `FileStore::Invalidate`

`sbc_file_result_test.cpp` gives them a minimal `SbcInterface`, `DataTransfer`, `FileStore`, `reprap` and semaphores.

The test checks what a file operation returns when the connection to the SBC ends while its request is in flight:
- The main task, or another task that reads or writes a file in SBC mode, posts the operation and waits on `fileSemaphore`
  (`DoFileOperation`). The SBC task (`TaskPriority::SbcPriority`) gives it when DSF has replied, and it stores the result in `fileSuccess`,
  `fileOffset` (the bytes read, or the file length of an open) and `fileHandle` first.
- When the connection is lost (`TaskLoop`) or the SBC starts a new one, e.g. after a restart of DCS (`ExchangeData`), `EndConnection`
  calls `InvalidateResources`, which gives `fileSemaphore` as well. The waiting task then reads the result members, and they must say
  that the operation failed. Before the fix they still held the result of the operation before: `ReadFile` returned the position of the
  last seek or the length of the opened file as the number of bytes read, and `FileStore::ReadLine` scanned and wrote past its buffer,
  e.g. the stack buffer of `HeightMap::LoadFromFile` (G29 S1). `OpenFile` returned the handle of the file before, and the others
  reported success.
- A background write (`WriteFileAsync`) that is in flight then is already resolved as failed (`FinishAsyncWrite(false)`), and a request
  whose wait times out returns the failure of each operation. The test checks these as well; they pass with and without the fix.

The test plays the earlier operations (an open and a seek that succeed) and the SBC task where the main task waits, then checks the
return value of each operation, the bytes behind a 16-byte line buffer, and that the file operation slot is free afterwards.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`). The firmware builds do not use this folder:
make and CMake only read `src/`, and a resource filter in `.project` hides it from Eclipse.

## Build and run (WSL, g++ 13)

    make check        # build/sbc-file-result-test and build/asan-sbc-file-result-test (AddressSanitizer + UBSan), then run both

Each case prints `PASS` or `FAIL` with the reason. The exit code is 1 if any case fails.
