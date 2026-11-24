# Host test of the answers to SetVariable and EvaluateExpression requests from the SBC that do not fit into a transfer: the real text of the two
# cases of SbcInterface::ExchangeData and of SbcInterface::MakeJsonAnswer (src/SBC/SbcInterface.cpp), of the DataTransfer functions that write
# the answers (src/SBC/DataTransfer.cpp and .h), of ObjectModel::ReportHeapArrayAsJson (src/ObjectModel/ObjectModel.cpp) and of the
# OutputBuffer functions they use (src/Platform/OutputMemory.cpp), extracted at build time, with the real RRFLibraries string and printf code.
# Run with make in WSL (g++ 13).
#
#   make              build build/sbc-variable-answer-test
#   make asan         the same with AddressSanitizer and UBSan, which stop at the first report, build/asan-sbc-variable-answer-test
#   make check        build both and run them
#
# Run it against the source without the fix to see the cases that fail there:
#   make clean check RRF=<checkout of the previous commit> RRFLIBS=<RRFLibraries> WITHOUT_FIX=1

RRF ?= ../..
SRC := $(RRF)/src
RRFLIBS ?= $(RRF)/../RRFLibraries

CXX ?= g++
CXXFLAGS ?= -O1 -g
# -include cstdio: SafeVsnprintf.h declares snprintf and vsnprintf without noexcept, glibc declares them with it, and glibc's
# declarations have to come first
# -Wno-format: the source prints size_t values with %u (32-bit target)
BASEFLAGS := -std=gnu++20 -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-format -include cstdio
INC := -Igen -I$(RRFLIBS)/src
ASANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

EXTRACT := ../macro-error-test/extract.awk
OUTPUT_FUNCTIONS := OutputBuffer::Length OutputBuffer::Clear OutputBuffer::UpdateWhenQueued OutputBuffer::vcatf OutputBuffer::catf \
	OutputBuffer::copy OutputBuffer::cat OutputBuffer::Init OutputBuffer::Allocate OutputBuffer::Release OutputBuffer::ReleaseAll
TRANSFER_FUNCTIONS := DataTransfer::WriteEvaluationResult DataTransfer::WriteEvaluationError DataTransfer::WriteSetVariableResult \
	DataTransfer::WriteSetVariableError DataTransfer::WritePacketHeader DataTransfer::WriteData DataTransfer::WriteDataHeader
TRANSFER_INLINES := DataTransfer::GetRxNumPackets DataTransfer::FreeTxSpace DataTransfer::CanWritePacket DataTransfer::AddPadding
SBC_FUNCTIONS := SbcInterface::MakeJsonAnswer
SBC_CONSTANTS := MinimumBuffersForSbcJson MaxSbcAnswerDataLength ValueTooLongForSbc
ifeq ($(WITHOUT_FIX),1)
# The source without the fix has none of the functions and constants that the fix adds, and its cases do not use them
SBC_FUNCTIONS :=
SBC_CONSTANTS :=
else
TRANSFER_FUNCTIONS += DataTransfer::GetSetVariableResultLength DataTransfer::CanWriteSetVariableResult
endif
LIBSOURCES := $(addprefix $(RRFLIBS)/src/General/,StringRef.cpp SafeVsnprintf.cpp Strnlen.cpp StringFunctions.cpp SimpleMath.cpp)
SOURCES := sbc_variable_answer_test.cpp $(LIBSOURCES)

GEN := gen/SbcConstants.inc gen/SbcCases.inc gen/SbcFunctions.inc gen/TransferFunctions.inc gen/TransferInlines.inc \
	gen/ReportHeapArrayAsJson.inc gen/OutputMemory.inc

.PHONY: all asan check clean
all: build/sbc-variable-answer-test
asan: build/asan-sbc-variable-answer-test

# The constants of the fix at file scope, which extract.awk does not handle: every line that defines one of SBC_CONSTANTS or asserts on it.
# Fails unless each of them is defined
gen/SbcConstants.inc: $(SRC)/SBC/SbcInterface.cpp Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(SBC_CONSTANTS)" '{ sub(/\r$$/, "") } BEGIN { n = split(names, w, " ") } \
		{ for (i = 1; i <= n; i++) if ($$0 ~ ("^(constexpr .*[ *]" w[i] " =|static_assert\\(" w[i] ")")) { printf "#line %d \"%s\"\n", NR, f; print; if ($$0 ~ /^constexpr/) found[w[i]] = 1; break } } \
		END { for (i = 1; i <= n; i++) if (!(w[i] in found)) { print w[i] " not found" > "/dev/stderr"; bad = 1 } exit bad }' $< > $@.tmp && mv $@.tmp $@

gen/SbcCases.inc: $(SRC)/SBC/SbcInterface.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="case:EvaluateExpression case:SetVariable" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/SbcFunctions.inc: $(SRC)/SBC/SbcInterface.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	$(if $(SBC_FUNCTIONS),awk -v f="$(abspath $<)" -v names="$(SBC_FUNCTIONS)" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@,echo "// no MakeJsonAnswer in this source" > $@)

# The definitions of static members start with a /*static*/ comment, which extract.awk does not take for a function head. sed removes the
# comment without changing the number of any line, so the #line directives stay right
gen/TransferFunctions.inc: $(SRC)/SBC/DataTransfer.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	sed 's#^/\*static \?\*/ ##' $< | awk -v f="$(abspath $<)" -v names="$(TRANSFER_FUNCTIONS)" -f $(EXTRACT) > $@.tmp && mv $@.tmp $@

gen/TransferInlines.inc: $(SRC)/SBC/DataTransfer.h $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="$(TRANSFER_INLINES)" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/ReportHeapArrayAsJson.inc: $(SRC)/ObjectModel/ObjectModel.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	awk -v f="$(abspath $<)" -v names="ObjectModel::ReportHeapArrayAsJson" -f $(EXTRACT) $< > $@.tmp && mv $@.tmp $@

gen/OutputMemory.inc: $(SRC)/Platform/OutputMemory.cpp $(EXTRACT) Makefile
	@mkdir -p gen
	sed 's#^/\*static \?\*/ ##' $< | awk -v f="$(abspath $<)" -v names="$(OUTPUT_FUNCTIONS)" -f $(EXTRACT) > $@.tmp && mv $@.tmp $@

build/sbc-variable-answer-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(if $(filter 1,$(WITHOUT_FIX)),-DWITHOUT_FIX) $(INC) $(SOURCES) -o $@

build/asan-sbc-variable-answer-test: $(SOURCES) $(GEN) Makefile
	@mkdir -p build
	$(CXX) $(BASEFLAGS) $(CXXFLAGS) $(ASANFLAGS) $(if $(filter 1,$(WITHOUT_FIX)),-DWITHOUT_FIX) $(INC) $(SOURCES) -o $@

check: all asan
	build/sbc-variable-answer-test
	build/asan-sbc-variable-answer-test

clean:
	rm -rf build gen
