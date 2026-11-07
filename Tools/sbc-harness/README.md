# SBC SPI link host harness

This harness replays recorded SPI exchanges against RRF's real SBC link code on a PC, without a board. It exists so that the
census of Duet Software Framework (DSF), which corrupts one SPI exchange at a time, can check its C# port of RRF's
DataTransfer/SbcInterface against the C++ itself. On the DSF side this is the test `PortMatchesRealFirmware` in
`src/UnitTests/SPI/LinkRecovery.cs`, which takes the path of a harness binary from `RRF_HOST_HARNESS`.

The Makefile reads the sources from this repository, `../..` (override with `RRF=`), so a rebuild always uses the checkout as it
is. The firmware builds do not use this folder: make and CMake only read `src/`, and a resource filter in `.project` hides it
from Eclipse, whose build configurations would otherwise compile `harness.cpp` into the firmware.

## Build and run (WSL, g++ 13)

    make              # build/sbc-harness-usb1 and build/sbc-harness-usb0
    make asan         # build/asan-sbc-harness-usb1/-usb0 (AddressSanitizer + UBSan)
    make check        # smoke test (a clean full transfer in both directions, then timeouts), link_test.py, boot_delay_test.py, restart_test.py

Two variants:
- **`sbc-harness-usb1`**, `SUPPORTS_SBC_OVER_USB=1`. This is the MB6HC release build: `Pins_Duet3_MB6HC.h` sets it to
  `CORE_USES_TINYUSB`, which is 1 for RTOS builds on SAME70 (CoreN2G `Core.h`).
- **`sbc-harness-usb0`**, `SUPPORTS_SBC_OVER_USB=0`.

On SPI the two differ in one place only, the wait while not connected (the `doingFullTransfer` case in `SbcInterface::TaskLoop`):
- usb1 waits `SbcConnectionTimeout`. When that runs out, `ResetConnection(true)` follows: sequence numbers go to 0 and TfrRdy is toggled again, every 4 s.
- usb0 waits `TimeoutUnlimited`.

Options:
- `-d`: RRF's own `debugPrintf` output (Module SbcInterface) goes to stderr.
- `-t`: harness trace (ResetConnection, EmergencyStop, platform messages) goes to stderr.
- `-n`: prints an `N <status>` line after `NEW` and `RESET`.

## Protocol (stdin/stdout, one line per event, stdout flushed after every line)

- **`NEW <skip> <underrun>`** starts a fresh instance.
  - The static SAME70 buffers, `dataReceived`, `transferReadyHigh`, the SPI counters and the PBUF arena are reset to their
    boot state, which is zero.
  - Then the `SbcInterface` members are constructed and `DataTransfer::Init()` runs.
  - Then the packet for transfer 1 is written, unless `skip == 1`.
  - Then the coroutine starts `SbcInterface::TaskLoop`, which runs `InitFromTask`, `StartNextTransfer`, `DoTransfer` and
    stops at the first wait. No output, unless `-n` is given.
  - `<underrun>` is a hex byte (`ff` or `0xff`).
- **`X <hex>`** is one exchange framed by NSS, with `<hex>` the bytes DSF clocks (`-` for none).
  - Output: `R <hex DSF reads> state=… pin=… conn=… disc=… resets=… wait=… got=…`. An empty read is shown as `-`.
- **`T`**: the wait TaskLoop is blocked in times out now.
  - Output: `T state=… pin=… conn=… disc=… resets=… wait=… got=…`.
  - During a `TimeoutUnlimited` wait, which never times out, the output is `E T during an unlimited wait` and nothing runs.
  - After `D` and during the boot (`RESET` until `NEW`) the output is `E T while the interrupts are off` or `E T during the boot`.
- **`D <hex>`** is an exchange like `X` while RRF's interrupts are off, as when `SoftwareReset` writes the reset data: the DMA
  moves the bytes, the NSS interrupt does not run and TaskLoop stays where it is. The interrupts stay off until `RESET`, so
  every further `X` behaves like `D`. The harness ends both DMA channels with that exchange, so a further one moves nothing
  (on the board a shorter exchange would leave the rest of the DMA armed).
  - Output: as for `X`.
- **`RESET <underrun>`**: the processor resets. The TaskLoop coroutine is abandoned, SPI, XDMAC and NVIC are in their reset
  state and TfrRdy keeps its level, as the census of DSF assumes. An `X` during the boot reads `<underrun>`, reaches nothing and
  shows `state=Boot pin=<level> conn=0 disc=0 resets=0 wait=boot got=-`. The boot ends with `NEW`, which runs
  `DataTransfer::Init`. No output, unless `-n` is given. A restart between two exchanges, before RRF arms the next one, is
  `D` (the exchange that completes), `RESET`, `NEW`; run the harness with `-b` and send `T` when the boot arm delay ends.
- **`END`** drops the instance. No output.
- Errors are `E <text>`: bad syntax, a command without an instance.

Status fields:

| Field | Meaning |
|---|---|
| `state` | `DataTransfer::state`, names generated from the `InternalTransferState` enum in `DataTransfer.h` |
| `pin` | last level written to the TfrRdy pin |
| `conn` | `SbcInterface::isConnected` |
| `disc` | number of `reprap.EmergencyStop()` calls (TaskLoop's disconnect handling) |
| `resets` | number of `transfer.ResetConnection()` calls from TaskLoop |
| `wait` | timeout of the `TakeIndexed` TaskLoop now waits in: `full` = SbcConnectionTimeout, `partial` = SbcTransferTimeout, `unlimited` = TimeoutUnlimited, else `<n>ms` |
| `got` | every `dsf:<digits>` found in the payload of the packets ExchangeData read in this step, or `-` |

Transfer numbering for the `rrf:<k>` packets:
- The packet for transfer 1 is written in `NEW`.
- The m-th call of ExchangeData writes the packet for transfer m+1, right before TaskLoop's `StartNextTransfer`.
- `k` counts the packets actually written, starting at 1.
- A packet is lost when `ResetConnection` clears the tx buffer. That is real RRF behaviour.
- The writer is `DataTransfer::WriteCodeReply(MessageType::UsbMessage, …)`, the function RRF uses for code replies.

## What is real and what is the harness

Compiled unchanged from RRF:
- `src/SBC/DataTransfer.cpp`: all of it, including `SBC_SPI_HANDLER`, `setup_spi`, `disable_spi` and `spi_dma_check_rx_complete`.
- `src/Storage/CRC32.cpp`: the SAME70 slicing-by-4 path.
- `src/SBC/DataTransfer.h`, `src/SBC/SbcMessageFormats.h` and `src/Platform/MessageType.h`.

Real text, extracted at build time by `extract-taskloop.awk`, with `#line` directives that point at the real lines:
- `SbcYieldTimeout` and `SbcBootArmDelay` from `SbcInterface.cpp`.
- `SbcInterface::TaskLoop` from the same file.

Harness code, not RRF:

| Replaced | RRF original | What the harness does instead |
|---|---|---|
| `SbcInterface::ExchangeData` | `SbcInterface.cpp` | The packet loop has the real shape: up to `PacketsToRead()`, stop on a null packet, stop on `request >= InvalidRequest` unless it is `ActivateLinkCapabilities` (0x4D50). That one is executed like the real case: `ReadData`, then `ActivateLinkCapabilities` if the length is at least 4. Every other packet is only scanned for `dsf:N` (`ReadData(packet->length)`). Then one `rrf:<k>` packet is written. Not reproduced: executing packets, resend requests (`ResendPacket`), object model/code buffer packets, and the delay between transfers (`TakeIndexed(maxDelayBetweenTransfers)` / `burstModeDelay`). During that delay RRF has not armed the next exchange yet. |
| `SbcInterface::InvalidateResources` | `SbcInterface.cpp` | Nothing. The real one does not touch DataTransfer: code buffer, files, G-code channels, heaters only. |
| `SbcInterface::Init` | `SbcInterface.cpp` | Only `transfer.Init()`. |
| `reprap.GetSbcInterface().IsConnected()` | `SbcInterface.h`, `RepRap.h` | DataTransfer.cpp sees a stand-in that has only `IsConnected()`, which returns TaskLoop's `isConnected`. |
| `SendUsbInitMessage` | `SbcInterface.cpp` | Empty. Not reached, because `pendingUsbDevice` is never set. |
| `TaskBase::TakeIndexed` / `GiveFromISR` | RRFLibraries/FreeRTOS | The notification count of index SbcInterface. A pending notification returns true at once. Otherwise TaskLoop blocks until the next `X` (whose NSS interrupt gives the notification) or `T`. Exception, `TakeIndexed(SbcYieldTimeout)` after a connection reset in TaskLoop: it returns false at once, because SPI is disarmed then (DoTransfer returned without setting up an exchange), so nothing can wake it. |
| SAME70 SPI1, XDMAC, NVIC, TfrRdy, PBUF pool | hardware, CoreN2G | See below. |

SPI model, as DSF specified it:
- The NSS interrupt runs if SPI is enabled, NSSR is enabled in IMR and the NVIC line is enabled. `spi_reset` clears IMR and the status.
- RRF receives `min(len, RX UBLEN)` bytes into the RX target. The rest of the target keeps its content.
- DSF reads the TX source for `min(len, TX UBLEN)` bytes and `<underrun>` beyond. It reads only `<underrun>` when SPI or the channel is disabled.
- The harness sets `SPI_SR_OVRES` when `len >= RX UBLEN + 2` and `SPI_SR_UNDES` when `len > TX UBLEN`. Both only feed RRF's
  M122 counters; the exact hardware conditions are not verified.
- The handler disables both XDMAC channels, so `spi_dma_check_rx_complete` (XDMAC_GS) is true and `finishingTransfer`
  never happens.
- The PBUF arena has the size of the MB6HC pool, 16 pbufs of 1552 bytes (24832 bytes), so the third 8 KB buffer of the sequence
  guard fits as on the board. It is zeroed at `NEW`. Bytes that DSF clocks beyond RRF's `txHeader.dataLength` come from this memory: zero
  after boot, later stale data from earlier transfers.

Host differences:
- 64-bit x86. DataTransfer.cpp and CRC32.cpp cast pointers to `uint32_t`, which needs `-fpermissive`; those warnings are
  expected. Linking with `-no-pie` keeps every DMA buffer below 4 GB, so the harness can turn the XDMAC address registers
  back into pointers; `main` checks this.
- `size_t` is 64-bit here. In DataTransfer's arithmetic this only matters on unsigned wrap-around, where comparisons give
  the same result.
- Cache maintenance calls are no-ops; the SAME70 buffers are non-cached.

## Link capabilities test

`link_test.py` drives one connection through the link capabilities: activation in transfer 1, RRF's switch to extended
numbers and codes in transfer 2, DSF's in transfer 3 (codes with 3 bit errors), phase guard rule A, a resend by the sequence
guard (DSF repeats transfer 3, RRF sends it again and then the waiting transfer 4), and DSF leaving the extended link (regular
codes at once, connection reset when the transfer ends, regular numbers that go on from the status bits).

## Boot arm delay test

`boot_delay_test.py` needs sources with `SbcBootArmDelay` (RRF PR #36) and runs the harness with `-b`, so that `delay()`
blocks until `T` and an `X` during it does not wake the task, as with vTaskDelay. Without `-b`, `delay()` only advances
the virtual time, so every other test and the port comparison see no difference. Checked:
- After `NEW`, TaskLoop is in the boot arm delay: `state=ExchangingData pin=0 wait=delay`.
- Exchanges during the delay (a 4-byte BadResponse, a whole header) reach the disabled SPI: DSF reads the underrun byte, RRF does not move.
- `T` ends the delay: the first header is armed with TfrRdy high.
- The first transfer is clean.
`extract-taskloop.awk` copies `SbcBootArmDelay` when it is there, so sources from before the delay still build.

## Restart test

`restart_test.py` replays the restart the lab printer showed on 2026-11-07 after `M122 P1001`: on the extended link, DSF
completes RRF's header exchange while RRF's interrupts are off (`D`), RRF resets and boots (`RESET`, `NEW 1 00`), DSF's
header response hits the boot window and reads zeros, and DSF's extended BadResponse lands in the first header the booted
RRF arms. The test reports one of two outcomes and checks each exchange of it:
- `loop`: RRF answers BadHeaderChecksum, and both sides stay one exchange apart. DSF reads `05000000` and zeros as the header
  and `0x0007005f` as the response, as in the printer's log.
- `in-step`: RRF decodes the extended BadResponse while it is not connected, retries the data response, and the next headers
  arrive whole. RRF's sequence number 1 shows DSF the restart, and the transfer ends in a connection reset without an e-stop.

`--expect loop|in-step` makes another outcome a failure. `make check` runs it without, because the outcome depends on the sources.
