# Host test of the result of an SBC file operation that was in flight when the connection ended: the real text of the main task's file
# functions, of the SBC task's send and result handling and of InvalidateResources (src/SBC/SbcInterface.cpp), and of FileStore::Read and
# FileStore::ReadLine (src/Storage/FileStore.cpp), extracted at build time, with the SBC task played in sbc_file_result_test.cpp.
# Run with make in WSL (g++ 13).
#
#   make              build build/sbc-file-result-test
#   make asan         the same with AddressSanitizer and UBSan, build/asan-sbc-file-result-test
#   make check        build both and run them

RRF ?= ../..
SRC := $(RRF)/src

CXX ?= g++
CXXFLAGS ?= -O1 -g
BASEFLAGS := -std=gnu++17 -fno-rtti -Wall -Wextra
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

SBC_FUNCTIONS := SbcInterface::FileExists SbcInterface::DeleteFileOrDirectory SbcInterface::SecureDeleteFile SbcInterface::GetFileList \
	SbcInterface::OpenFile SbcInterface::ReadFile SbcInterface::WriteFile SbcInterface::WriteFileAsync SbcInterface::WaitForAsyncWrite \
	SbcInterface::SeekFile SbcInterface::TruncateFile SbcInterface::CloseFile SbcInterface::WaitForFileSlot SbcInterface::PostFileOperation \
	SbcInterface::DoFileOperation SbcInterface::FinishAsyncWrite SbcInterface::InvalidateResources
SBC_RESULTS := case:CheckFileExistsResult case:FileDeleteResult case:OpenFileResult case:FileReadResult case:FileListResult \
	case:FileWriteResult case:FileSeekResult case:FileTruncateResult
FILESTORE_FUNCTIONS := FileStore::Seek FileStore::Position FileStore::Read FileStore::ReadLine FileStore::Invalidate

GEN := gen/SbcFileFunctions.inc gen/SbcFileResults.inc gen/SbcSendFileOperation.inc gen/FileStore.inc

.PHONY: all asan check clean
all: build/sbc-file-result-test
asan: build/asan-sbc-file-result-test

gen/SbcFileFunctions.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(SBC_FUNCTIONS)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/SbcFileResults.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(SBC_RESULTS)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/SbcSendFileOperation.inc: $(SRC)/SBC/SbcInterface.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="if:fileOperationPending.load" -f extract.awk $< > $@.tmp && mv $@.tmp $@

gen/FileStore.inc: $(SRC)/Storage/FileStore.cpp extract.awk Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(FILESTORE_FUNCTIONS)" -f extract.awk $< > $@.tmp && mv $@.tmp $@

build/sbc-file-result-test: sbc_file_result_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) -Igen sbc_file_result_test.cpp -o $@

build/asan-sbc-file-result-test: sbc_file_result_test.cpp $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) -Igen sbc_file_result_test.cpp -o $@

check: all asan
	build/sbc-file-result-test
	build/asan-sbc-file-result-test

clean:
	rm -rf build gen
