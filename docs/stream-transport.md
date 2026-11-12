# Shared stream transport

Clipboard dongle transfers, clipboard LAN transfers, PC-fed OTA and S3-cached
receiver OTA use bounded stream delivery. Clipboard bundles and firmware endpoints
keep their own verification and commit policy. Direct esptool flashing is separate.

## Caller interface

Python callers use `kms_transport.send_stream(source, size, destination, ...)`.
The destination adapter handles opening/resuming, framing, receipt identities,
retries and final commit. The caller supplies a readable source, destination,
optional profile, progress callback and a cancellation check that can raise.
Packet sizes, HID reports, ESP-NOW MTUs and queue capacities stay in the adapters.
The current file/firmware adapters use seekable sources for resumption.

The sender reads at most one negotiated window ahead, retains unacknowledged data,
and reports only committed progress. Clipboard keeps four 160-byte cells in flight;
OTA keeps four 188-byte writes when both ends support streams and one on older
firmware. Local USB capacity reports stop submission when firmware bulk slots are
full. The firmware pool retains slot ownership if the radio driver rejects a send.
Receiver queue pressure rejects a complete batch; endpoint receipts and bounded
retries recover it without accepting a partial envelope.

Clipboard retries back off from 0.8 through 1.6 to 2 seconds, with a 30-second
no-progress deadline. PC OTA uses the same pump with a 45-second deadline; cached
OTA uses a bounded four-write exchange with a 30-second deadline. Firmware image
validation, confirmed reboot, activation and rollback checks still decide success.

## Speed profiles

Set **Dongle speed** in Companion Clipboard preferences or select the speed in
Firmware Setup before an application update. Maintenance CLI examples:

```text
kms-firmware --bundle firmware.zip ota --role receiver --port COM8 --slot 2 --speed balanced
python -m kms_update.management --port COM8 --speed fast update --slot 2 --cached
```

| Profile | Bulk PHY rate | Minimum gap between bulk sends | Maximum batch |
| --- | --- | --- | --- |
| Conservative (default) | 1 Mbps | 10 ms | 2 cells |
| Balanced | 6 Mbps | 5 ms | 4 cells |
| Fast | 12 Mbps | 2 ms | 4 cells |

These are radio settings, not file throughput promises. Three consecutive failed
or slow (at least 100 ms) bulk completions step that peer down one profile. A new
boot or explicit profile change resets the choice. Input/control transmissions use
the existing 1 Mbps rate; pending input/receipts and route activation defer bulk.
Bulk failures use transport fallback rather than triggering receiver recovery.
The Companion status exposes requested/local effective profiles; each radio hop
can fall back independently. The transfer window labels the local hop accordingly.

New firmware exchanges a small boot-bound `KMB2` capability handshake over the
paired unicast link. Only a confirmed peer receives larger batches. Unknown or old
peers continue to receive legacy `KMSM` cells. The existing management envelope
stays at 226 bytes; a versioned batch is at most 940 bytes and contains at most four
validated cells. No global management/console buffers or input queue limits grow.
Receiver feature report 36 reports pool capacity and selects the profile; management
operation 22 provides the same configuration/status for maintenance clients.

## Memory ownership

The input-priority gate applies to every outgoing stream frame, including
clipboard advertisements and capability replies. A free MAC transmit slot while
an input acknowledgement is outstanding is not permission to start another
stream send. Deferred frames retain their queue slots and resume when input
allows them; this does not change the priority of ordinary desktop management.

Clipboard and ordinary management output share one fixed slot pool. Clipboard's
former decoded-command and output queues are removed; automatic recovery retains
one separately validated control slot. Two shared slots are reserved for control
traffic, including cancellation. Firmware receive queues stay bounded at four
cells. No new worker tasks or task stacks are added.

The S3 has eight shared slots; the S2 has six, of which at most four hold bulk data.
Pool cells, the reusable batching workspace and the four-entry duplicate-reply
cache use the existing PSRAM-preferred allocator on the S3. The S2 uses internal
RAM. The reply cache previously retained eight complete request/reply pairs in
static internal RAM. Allocation fallback uses the same fixed counts, never an
unbounded internal pool. A single 940-byte internal TX staging buffer crosses the
Wi-Fi driver boundary. Cached-image reads and OTA writes use internal worker-stack
buffers, keeping flash operations away from PSRAM-backed data pointers.

The installed S3 is N16R8 (8 MiB PSRAM). The S2FH4 receivers have no PSRAM. Firmware
build sizes measure linked static storage; actual free/minimum heap and task-stack
headroom must also be checked on running hardware, including S3 PSRAM-unavailable
fallback. No physical RAM/throughput measurements are implied by native tests.

## Verification

`scripts/test-stream.py` exercises the production codec, pool, negotiation, driver
rejection, boot changes, full-batch admission, input priority and profile fallback.
It also checks asynchronous OTA forwarding and matched receipts. Native cache
tests cover both legacy and windowed cached-source delivery. Python stream tests
bound source read-ahead and retries, cancel during backoff, reject impossible
receipts and recover OTA gaps without changing retry identities. Existing clipboard
loss/reordering/integrity, management, rollback, USB and packaging suites still run.

Physical acceptance requires both firmware roles and both Companion apps updated,
then transfers in both directions under mouse/keyboard use, poor-signal tests,
cancellation, old/new peer combinations, cached/PC-fed OTA and heap/stack samples.
The 115200-baud hexadecimal maintenance console remains a PC-fed OTA bottleneck;
cached OTA avoids repeatedly uploading the image from the PC. Higher RF rates
alone cannot remove the UART, USB, flash and application acknowledgement costs.

## USB polling throughput follow-up

A live Fast transfer on the Windows sender measured 5.25 KiB/s with a median
29.17 ms helper service cycle. The old 2 ms USB work budget usually allowed just
one 160-byte cell per cycle: about 5.36 KiB/s at that cycle time. Both receivers
reported zero failed/rejected radio transmissions, and the refreshed sending S2
journal showed no current-boot transfer faults. These are observations from the
previous build, not measurements of this follow-up.

Credit-capable dongles now receive a 12 ms soft USB work budget per direction,
with at most four reads/writes and the existing pool-credit bound. An individual
HID control call cannot be interrupted, so the budget can overrun by one call.
Input still travels through its existing firmware path; long HID work does not
add packets to its radio queue. Very old firmware without credits keeps the
previous conservative polling limit.

New S2 firmware packs a tagged mailbox snapshot into the existing status report's
otherwise unused identifier: tag 0x53, incoming count, free bulk slots, effective
local profile (255 means legacy/unnegotiated). Its existing CRC covers the word.
The companion uses it to skip empty clipboard reads and the extra capacity GET.
Profile changes still use feature report 36, once per change. Older companions
ignore this identifier; older receivers retain the feature-36 fallback. No USB
report, endpoint, queue, packet window or firmware buffer grows.

The latency-aware USB model moves submission capacity from 6.25 to 14.88 KiB/s
(2.38x) with 6 ms feature transfers. This is a host-submission model; radio,
receipts and the destination can lower actual file speed. New physical measurements
are needed after installation on both PCs. Much higher sustained throughput would
require reducing per-packet HID overhead or a larger flow-controlled window.
