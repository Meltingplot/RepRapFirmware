# SBC object model overflow host test

This test checks what RRF does with a `GetObjectModel` request from the SBC whose response does not fit into the output buffers.

## Why

On the lab printer (MB6HC, 3.7.0+1.mp.9, SBC over SPI) the lines

    global m = vector(128,vector(128,vector(20,10)))
    global m = vector(128,vector(128,vector(60,10)))

stopped the object model of DSF. After a `global` RRF increments the sequence number of the key `global`
(`SbcInterface.cpp`, case `SetVariable`, `reprap.GlobalUpdated()`), and DSF asks for the key with the flags `d99v` (DSF
`Model/UpdateService.cs`). The key is an object, so the array in it is no root array, and `ReportHeapArrayAsJson` does not cut it after
half the buffer space. Its JSON is about 1 MB for n = 20, while a response may use 36 buffers of 256 bytes (`Configuration.h`, 40 buffers,
4 reserved). The DCS journal and M122 showed:
- DSF sent the request again forever: "Resending packet #0 (request GetObjectModel)" every 1.75 s for n = 20, 2.9 s for n = 35,
  3.3 s for n = 40. The object model of DSF stopped, and the SBC task took 96.5 % of the CPU, the slowest main loop was 5.2 s.
- For n = 60 one attempt took more than 4 s, and DSF lost the connection ("Connection timeout while waiting for TfrRdy pin
  (waited 4000ms)").

Two causes:
1. `RepRap::GetModelResponse` returns null when the response overflows the output buffers (since a71991c4b, 2020). M409, rr_model and the
   PanelDue status report take the object model report mutex, wait while fewer than `MinimumBuffersForObjectModel` buffers are free, and
   treat a null response as too long (`GCodes2.cpp` M409: "We don't delay and retry here, in case the user asked for too much of the
   object model in one go"). The case `GetObjectModel` of `SbcInterface::ExchangeData` did neither and took a null response for "try
   again later", so DSF sent the request again and RRF built the response again, forever.
2. `ObjectModel::ReportHeapArrayAsJson` went on through every element after the buffers had overflowed, about 5 us per integer on the
   board, although the response was discarded. A nested array holds its elements by reference (vector() fills the array with the same
   element), so it can stand for millions of them.

The fix:
1. The case takes the object model report mutex like the other three and waits while fewer than
   `MinimumBuffersForSbcJson` = `RESERVED_OUTPUT_BUFFERS` + `SbcTransferBufferSize` / `OUTPUT_BUFFER_SIZE` = 36 buffers are free.
   A response may use all buffers except the reserved ones (`OutputBuffer::Allocate` with allowReserved false), so with that many free a
   null response is longer than a transfer and can never be sent. The case then discards it like the excessively long response that it
   already discarded (an empty response and an error message), instead of asking DSF to send the request again. The threshold is not
   `MinimumBuffersForObjectModel` (20), because 20 buffers hold only 5 KB, and a response of up to 8 KB that the SBC could take would
   then be discarded while other output holds buffers, where it waited before.
2. `ReportHeapArrayAsJson` stops when the buffers have overflowed.

Behaviour change: an object model response that can never fit into a transfer is discarded with "Cannot store excessively long object
model response, discarding request (output buffers full, key ..., flags ...)" instead of being requested again forever. DSF logs
"Invalid key global in the object model" for it and keeps its old copy of the key. The SBC task now also waits while another task reports
the object model or fewer than 36 buffers are free, where it built the response at once before.

Upstream code: the case is the same in Duet3D 3.7-dev. Found on the lab printer on 2026-11-24.

## What the test compiles

The real text of the case `SbcRequest::GetObjectModel` of `SbcInterface::ExchangeData` and of the constant `MinimumBuffersForSbcJson`
with its `static_assert` (`src/SBC/SbcInterface.cpp`), of
`RepRap::GetModelResponse` and `RepRap::StartJsonResponse` (`src/Platform/RepRap.cpp`), of `ObjectModel::ReportHeapArrayAsJson`
(`src/ObjectModel/ObjectModel.cpp`) and of the `OutputBuffer` functions they use (`src/Platform/OutputMemory.cpp`: `Allocate`, `cat`,
`copy`, `catf`, `Release`, ...), which the Makefile extracts with `../macro-error-test/extract.awk` (with `#line` directives that point at
the real lines). The output buffers are the real pool of 40 buffers of 256 bytes with 4 reserved, and `catf` is the real `vuprintf` of
RRFLibraries. `sbc_om_overflow_test.cpp` has the class declarations, a `DataTransfer` whose `WriteObjectModel` takes and releases the
buffers like the real one, a mutex that the test can mark as held by another task, and an object model whose key `global` holds one
variable `m`, reported as RRF reports the global variables: an object, with integers written as `ReportItemAsJson` writes them and arrays
with the real `ReportHeapArrayAsJson`. Arrays hold their elements by reference, as in RRF.

The cases: a small global; the globals of the lab printer with n = 20, 30 and 60; a response of 8649 bytes, which fits into the buffers but
not into a transfer (discarded as before, with its length); 35 and 36 free buffers; the mutex held by another task; a transfer without room
for the response or for the empty response (the request waits, as before); an unknown key (the error goes back as the response). Every
case also checks that all buffers are free again afterwards.

Without the fix 7 of the 12 fail: the three globals of the lab printer and the large one with 36 free buffers are not acknowledged, so DSF
sends them again, and each attempt reports all 327680, 491520 or 983040 integers; the two cases that must wait (35 free buffers, mutex
held) do not; and the large global in a full transfer reports all its integers. With the fix all 12 pass.

Not covered: the board itself (the time per element and the TfrRdy timeout), DSF's handling of the empty response, and the SetVariable
and EvaluateExpression replies that are larger than a transfer (separate fix).

## Build and run (WSL, g++ 13)

    make check        # build the plain and the AddressSanitizer + UBSan program, then run both
    make clean check RRF=<checkout without the fix> RRFLIBS=<RRFLibraries> WITHOUT_FIX=1
