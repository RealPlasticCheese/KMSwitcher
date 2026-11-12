# Slave diagnostics and firmware updates

The master's UART is the control point for receiver diagnostics and application
updates. Receivers continue exposing USB HID to their PCs. Management uses the
existing encrypted ESP-NOW peers and a separate protocol/queue from input traffic;
the HID wire format remains v2 and pairing records retain their current format.

Management frames, including every OTA operation, must be unicast to the board's
own station MAC, then pass the saved peer MAC/slot checks. ESP-NOW permits
unencrypted broadcasts even from an encrypted peer's source MAC; those broadcasts
are rejected before entering the management queue. Operational HID/control frames
also require unicast. Only pairing requests and independently HMAC-authenticated
channel discovery use broadcasts. Pairing still requires a physical pairing window.

This boundary relies on the pinned ESP-IDF driver's encrypted-unicast peer checks.
It does not protect against a compromised paired master or extracted pairing key,
and image hashes/format checks do not authenticate the firmware publisher. Signed
firmware and secure boot are not enabled by this change. Update each S2 to apply
its OTA receive protection; updating only the S3 does not protect older receivers.

This implementation has automated/fake-device coverage and builds for both chips.
No working device has been migrated or wirelessly updated as part of this change.
Complete the hardware checklist below on a spare receiver before fleet rollout.

## Build identity

Every production build automatically embeds the checked-out Git commit ID and a
UTC timestamp. Uncommitted tracked or non-ignored untracked files add `-dirty` to
the Git identity. The commit is the source used by that build, including a feature
branch build; it is not a lookup of the newest remote `master`. Named releases or
manual version increments are unnecessary.

Builds also produce `build/controller/kmswitcher_controller.json` and
`build/receiver/kmswitcher_receiver.json` alongside their `.bin` files. A manifest
records the role, chip, Git identity, UTC timestamp, exact file size and SHA-256 of
the entire `.bin`, including its appended ESP image digest. Keep each binary and
its matching JSON together. The updater checks their agreement before opening
the serial port. `about` displays the master's Git/UTC identity, binary fingerprint,
ELF fingerprint, SDK and protocol information. ELF and `.bin` hashes are distinct.

## Master console

Connect the S3's COM/UART connector and run `scripts/watch-controller.ps1` as in
[the console guide](recovery-and-diagnostics.md#open-the-console-from-powershell).
Use `help` for the full reference, including existing routing and history commands.

| Command | Result |
| --- | --- |
| `slaves` | Paired slots, contact age, cached identities and compact health summaries |
| `slave-status N` | Fetch that slave's current identity, health, totals and retention limits |
| `slave-history N` | Download an immutable snapshot and print it only after its CRC verifies |
| `slave-history-clear N` | Clear that slave's retained events/checkpoints in RAM and flash |
| `pause` | Send an input release, pause routing and remember the current destination |
| `resume` | Restore the previous destination if available; keep an intervening user selection |
| `remote N HEX` | CRC-framed transport endpoint used by the PC helper |
| `transfer-status` | Report whether queued or active firmware work blocks automatic handover |

`N` is an explicit paired slot from 1-7. A cached identity/summary is labelled with
its age; it is not proof the receiver is still reachable. Unsupported old firmware
and powered-off receivers remain listed as paired and queries fail with a bounded
timeout. A fresh status request confirms the running receiver boot and image.

Automatic switching resumes after confirmed cached updates, skipped updates or
definitive failures, and after a direct upload is activated or aborted. Cache inspection and
progress reads do not extend a transfer lease. Queued and active writes remain
protected; an upload with missing replies retains its normal inactivity
timeout until a terminal response or a new receiver boot is observed. This avoids
the earlier two-minute handover lockout after completed updates. `KMSTRANSFER`
traces show busy/clear transitions, queued work, cache activity and a bit mask of
direct receiver uploads. Controller `status` also reports the update guard.

The Stress Toolkit's **Background** preset issues read-only cache identity and
absent-token progress queries during input verification, then requires
`transfer-status` to report a clear guard. It performs no firmware write or reset.
Use S3 test firmware including this command; older firmware is reported as unable
to verify this regression. Actual update, timeout and immediate post-confirmation
handover paths are also exercised by the native management/desktop tests.

Information and transfer output runs on background workers. The application
processes HID before management traffic and sends at most one management frame
per configured pacing interval. Flash operations can still pause cached code on
the chip; input responsiveness under flash writes needs hardware measurement.

[Companion → Diagnostics](installing.md#device-management-in-companion) can
inspect remote receivers and verify retained-history downloads through the
attached dongle. Use [Firmware Setup](installing.md#standalone-firmware-setup)
for updates; master updates use a [local UART endpoint and OTA migration](master-updates.md).

## PC helper and archived logs

`manage-slaves.cmd` uses the pinned ESP-IDF Python, including pyserial, and forwards
arguments to `scripts/manage-slaves.py`. It discovers one CH343 bridge if `--port`
is omitted; specify a port when several bridges are connected. It leaves DTR and
RTS deasserted and does not reset the master. Close the interactive serial monitor
first: one process owns the COM port at a time.

```powershell
.\manage-slaves.cmd --help
.\manage-slaves.cmd --port COM8 list
.\manage-slaves.cmd --port COM8 status --slot 3
.\manage-slaves.cmd --port COM8 history --slot 3 --output logs\slave3-history.txt
.\manage-slaves.cmd --port COM8 clear --slot 3
.\manage-slaves.cmd --port COM8 restart --slot 2
.\manage-slaves.cmd --port COM8 restart --all
```

## Restart a dongle through the S3

In the Companion, choose **Restart dongles…** (also available from its tray menu),
select the S3 UART port, and **Inspect paired dongles**. Choose a numbered dongle
or **All paired dongles**, then review **Restart selected dongle(s)**. The standalone
setup tool opens at **Restart dongles**. No firmware bundle is needed for restarting.

Both S3 and S2 firmware need restart capability (identity capability bit 16).
Older firmware is reported as unsupported; offline dongles are reported individually
and do not stop a batch. A completely unresponsive radio still requires a physical
reset. Restarting briefly disconnects the dongle's USB HID; pairing and settings
are retained. The S3 remains running.

The tools fetch a fresh identity, send operation 20 with the current receiver boot
and the four-byte little-endian key `0x52535431`, then verify a different confirmed
boot running the same firmware. The command uses the existing encrypted unicast
peer/slot checks. Receivers acknowledge before a 1.5-second restart delay and release
USB input before resetting. Duplicate requests keep the original deadline. Firmware
updates and an unconfirmed boot report busy. Controller pairing/channel changes
also prevent restart; finish those operations before retrying. A lost acknowledgement triggers boot verification without a new restart
command. The selected receiver is processed last; routing is paused only if that
receiver is still selected, then restored while preserving an intervening selection.
An already paused route stays paused.

The S3 logs `KMSRESTART` with slot, boot, request ID and acceptance result. Acceptance
alone does not prove a restart: the tools report success only after verifying the
new boot, and expose per-dongle failures and route restoration warnings.

Downloads verify every chunk plus the complete snapshot CRC before writing the
output file. New records can continue entering the slave's journal during the
download. The snapshot is bounded, immutable and expires after inactivity. If
the slave restarts, the snapshot expires, or storage cannot supply a complete
snapshot, the download fails rather than presenting a partial file as complete.
PC archives can be kept indefinitely; dongle storage remains bounded.

## Radio evidence across restarts

Updated S2 firmware retains failed transmit completions, slow transmissions,
receive gaps associated with lease expiry, delayed receive dispatch, main-loop
gaps and restart markers in its diagnostic journal. Radio callbacks update RAM;
the management worker performs flash writes. Events coalesce per reason for
five seconds, with a statistics checkpoint for the first occurrence of each
reason and then at most once per 30 seconds. The cumulative `radio_failed` field
counts failed MAC completions separately from immediate send-submission errors.
It uses a previously reserved word in journal version 2, preserving its layout
and existing history.

Tagged radio events append `radio_reason`, `op`, `storage_busy` and `duration_ms`.
Reasons are 1=MAC failure, 2=slow transmit, 3=receive gap, 4=dispatch delay,
5=main-loop gap, 6=manual restart and 7=automatic restart. Durations saturate at
65,535 ms. Known diagnostic flash activity suppresses timing-only events and
marks any coincident MAC failure with `storage_busy=1`, avoiding a feedback loop
where saving diagnostics creates more urgent saves. These observations help
localize a stall; a receive gap alone does not establish bad radio reception.

Before accepting an automatic restart, the S2 checkpoints and saves its journal.
If that fails, it returns a storage error and stays running. A manual restart
still proceeds after best-effort saving. Power loss can lose evidence whose
background save has not completed.

Identity capability bit 32 advertises operation 21, a read-only live radio
snapshot. Companion inspection, the management CLI and browser status expose it
as `radio`, or report a diagnostic error. The 80-byte payload contains twenty
little-endian 32-bit words:

| Words | Values in order |
| --- | --- |
| 0–4 | Schema (1), receiver boot, sample uptime, TX submitted, TX completed |
| 5–10 | TX MAC failures, TX completions ≥100 ms, TX rejected, maximum TX ms, last TX ms, pending TX age ms |
| 11–16 | RX packets dispatched, last RX age ms, maximum RX gap ms, maximum dispatch delay ms, last dispatch delay ms, maximum main-loop gap ms |
| 17–19 | RX queue depth, USB ready, minimum free heap bytes |

Clients validate the schema and receiver boot. Live maxima reset on restart;
retained events/checkpoints provide the earlier boot's evidence. No input contents
are included. The older 168-byte statistics reply stays unchanged.

The [S3 automatic recovery policy](recovery-and-diagnostics.md#automatic-receiver-restarts)
also supports older S2 firmware advertising restart bit 16, using the existing
four-byte operation-20 request. Such firmware lacks the new persisted evidence.
For bit-32 receivers, automatic requests append the little-endian marker
`0x4155544f` after the restart key. Boot binding, authentication and duplicate
request handling still apply. Update both roles to obtain the complete feature;
pairing records are preserved.

## Retention and clearing

The companion can also [view the attached S2's retained events directly over USB](screen-edge-switching.md#local-dongle-logs-and-recovery-notifications),
including while its radio connection is unavailable. This is a read-only event
view; full statistics/checkpoint downloads remain available through the S3 tools.

Slaves retain 256 event records and 24 cumulative statistics checkpoints by
default. Both are circular journals: a new record replaces the oldest when full.
Status shows retained counts, capacities, overwrite counts and the oldest event's
boot ID/uptime. Consecutive identical events group into a record with first/last
uptimes and an occurrence count. Live totals preserve occurrences even when their
individual event records have rolled out of the ring.

Events include boot/reset reason, USB mount/unmount/suspend/resume, USB stalls and
queue overflows, radio receive overflow, lease expiry, output release, radio send
errors, update progress/failure and storage errors. Statistics include radio RX/TX,
duplicates/rejections, accepted input samples, completed USB reports, queue peaks,
maximum USB sample latency, minimum free heap, USB state and recent/mean RSSI.
Accepted samples and completed USB reports have different units: one sample can
produce keyboard, mouse and media reports, including forced neutral reports.
USB completion does not prove an application processed the input.

Live counters are cumulative within the current boot and reset when the receiver
restarts. Previous checkpoints retain their own boot's totals until overwritten
or explicitly cleared. No key values, pointer coordinates or input replay buffers
are recorded in the diagnostic journal.

The dedicated `kms_diag` NVS partition is separate from pairing NVS. New events
request a background save, coalesced over at least one second to bound writes
during bursts; grouped repeats are checkpointed periodically and
statistics checkpoint hourly. NVS reuses old blob versions; RAM/flash allocations
have fixed bounds. Storage failures leave RAM diagnostics/input running and are
reported in totals. Initialization never silently erases a failing partition.
Power loss before a save finishes can lose recent records/statistics.

`slave-history-clear N` clears events, saved checkpoints and retention/overwrite
counts, while preserving live counters, event sequence continuity, firmware and
pairing. It invalidates snapshot/read caches too. The same worker serializes all
writes and clearing, preventing a queued old write from restoring cleared history.
Success is acknowledged only after the cleared journal is committed. A failed
clear can clear RAM without completing the flash change; retry it before relying
on the result after a reboot. Explicit clear can repair a failing dedicated history
partition; it never erases pairing NVS.

## Updating one or all receivers

If an upload fails or reports a receiver restart, save the update report and collect
baseline status and retained device history before retrying.

Build receiver firmware normally, then supply its binary and adjacent JSON:

```powershell
.\scripts\idf.ps1 -Project receiver
.\manage-slaves.cmd --port COM8 update --slot 3 --firmware build\receiver\kmswitcher_receiver.bin
.\manage-slaves.cmd --port COM8 update --all --firmware build\receiver\kmswitcher_receiver.bin --report logs\update-results.json
.\manage-slaves.cmd --port COM8 update --all --firmware build\receiver\kmswitcher_receiver.bin --force
```

The helper queries each target and skips an identical `.bin` hash by default.
`--force` reinstalls matching firmware; it never bypasses chip, size, CRC, hash,
image format or boot-health checks. There is no release-number/downgrade ordering:
a different compatible supplied image is eligible for installation.

`--all` means all paired slots, including currently unreachable ones. Updates run
sequentially and report updated, skipped, unreachable or failed for each target;
one receiver failure does not prevent attempts on the others. The selected
receiver is processed last. If it needs writing, the helper pauses that route and
releases held input, then attempts to resume it afterward. A skipped selected
receiver does not pause input. Updates to inactive receivers leave the active
route running. Reaching an explicit user-selected route while paused prevents
`resume` from overriding that selection. A receiver with an active input lease
rejects the start of an update until the release/lease expiry has completed.
While an upload is active, the slave advertises USB unavailable for routing and
rejects new input activations; its physical USB connection remains enumerated.

Close the browser serial connection or other serial monitors before running the
command-line helper. To match a master configured for a different UART speed,
put `--baud SPEED` before `update`.

On older controllers using the streaming path, the PC loads the binary into
memory for validation, then sends one 188-byte
firmware chunk at a time and waits for its acknowledged next offset. The master
forwards these chunks over ESP-NOW; the master does not need to store an
entire firmware file. Each envelope covers its operation, slot, request/boot IDs,
offset, size and payload with CRC32. Retries reuse request IDs, and the slave
caches replies. Both clients allow up to eight attempts for each write (rather
than the three attempts used for ordinary requests), reusing the exact frame so
a lost acknowledgement cannot cause a second flash write. At the default
six-second timeout this is bounded to about 48 seconds per stalled chunk, below
the receiver's two-minute inactivity expiry. A permanent loss still fails and
reports the byte offset; this does not resume across restarts. Upload chunks
must be in order; the last exact duplicate is
acknowledged without writing again. Boot IDs and upload tokens prevent continuing
an interrupted session against a restarted receiver.

The slave writes the inactive OTA partition, reads back the exact image bytes,
checks the PC's SHA-256 and validates the ESP image/chip/project before selecting
it for boot. Incomplete or corrupt downloads never activate. An inactive transfer
times out and is abandoned; after a master restart, an owned transfer may need
that timeout before a fresh upload can start. A retry begins from offset zero;
cross-reboot transfer resumption is not implemented.

After activation, the slave reboots. A pending image confirms only after the
application loop remains alive, USB initialization has completed and paired
management traffic demonstrates working radio, with a configured startup grace
period. A valid running-image identity is also required. A pending image rolls
back immediately if its management worker cannot start. It rolls back if
confirmation otherwise fails by its deadline; the bootloader also
handles a reset/crash before confirmation. This tests initialization and radio
reachability, not OS/application input delivery. The helper reports success only
after a different boot ID reports the expected hash and successful confirmation,
including when `--force` reinstalls the same image.

## Firmware stored on the S3

Controllers with the expanded cache partition table retain the receiver firmware
from their release build. The cache uses two 2 MiB data slots; each holds one
receiver application up to the S2's existing `0x1e0000` OTA limit. A failed or
interrupted replacement preserves the previous verified image. An S2 update still
writes and verifies the receiver's inactive application slot and requires a new,
confirmed boot. A matching image is skipped unless `--force` is specified.

```powershell
.\manage-slaves.cmd --port COM8 cache-status
.\manage-slaves.cmd --port COM8 cache-put --firmware build\receiver\kmswitcher_receiver.bin
.\manage-slaves.cmd --port COM8 update --slot 3 --cached
.\manage-slaves.cmd --port COM8 update --all --cached --report logs\cached-update-results.json
```

`cache-put` replaces only the stored S2 image; it does not update any dongles or
the S3 application. `update --cached` needs no firmware files on the PC. Normal
`update --firmware FILE` automatically stores the image once and reuses it across
the batch when the S3 supports caching; older controllers retain the streaming
path. The packaged Firmware Setup uses the cache for receiver OTA too.

In desktop Firmware Setup, select the controller UART method and **Receiver
firmware source → Receiver firmware stored on S3**. Inspect to display the cached
image, then tick the dongles to update. This source needs no local firmware files
and never stages a replacement. The S3 controller stays unchecked and unchanged.
The installed CLI also accepts `kms-firmware ota --port COM8 --role receiver
--slot 3 --cached`.

In the browser, use **Inspect stored S2 firmware**, **Store selected S2 firmware
on S3**, or choose **Firmware stored on the S3** as the receiver firmware source.
Selected receiver files are cached once before a batch on supported controllers.
Controller JSON manifests carry their release's receiver image, refreshed after a
successful master application check. See [master updates](master-updates.md#receiver-firmware-bundled-with-the-master).

The S3 sends every firmware chunk and handles acknowledgement/retry, activation
and boot confirmation itself. The PC polls progress instead of forwarding each
chunk. Keep the client connected for results and route restoration; if the PC
disconnects, the current receiver continues updating on the S3, but later batch
targets are not started. Inspect the receiver and select/resume input after
reconnecting. A master restart interrupts the operation; cross-reboot resumption
is not supported. An inactive cache upload expires after two minutes.

Input to other receivers continues normally. The selected receiver is updated
last and paused only if writing is needed; route restoration respects an
intervening selection. Updating the radio or master application is blocked during
the cached transfer. Real speed improvement depends on radio/flash latency and
the serial speed previously used; it has not yet been measured on hardware.

## One-time wired migration and recovery

Install the new master firmware through its usual wired wrapper. Each existing
S2 also needs one wired installation of the OTA-capable bootloader, partition
table and application; its current single-app firmware cannot receive this
bootstrap wirelessly. Start with the spare dongle and check actual ports/MACs.

```powershell
.\scripts\idf.ps1 -Project controller -ResetConfig
.\scripts\idf.ps1 -Project receiver -ResetConfig
.\flash-controller.cmd COM8
.\flash-receiver.cmd COM4 -ExpectedMac aa:bb:cc:dd:ee:ff
```

The wrappers use generated flash arguments; do not substitute old offsets or run
an erase-flash operation. New layouts preserve pairing NVS at `0x9000` for
`0x6000` bytes and PHY data at `0xf000`. The S3 keeps its app at `0x10000` with a
larger partition. The S2 uses this 4 MiB layout:

| Partition | Offset | Size |
| --- | --- | --- |
| Pairing NVS | `0x9000` | `0x6000` |
| PHY | `0xf000` | `0x1000` |
| OTA selection data | `0x10000` | `0x2000` |
| OTA application 0 | `0x20000` | `0x1e0000` |
| OTA application 1 | `0x200000` | `0x1e0000` |
| Diagnostic NVS | `0x3e0000` | `0x20000` |

Pure OTA application updates preserve all data partitions. Re-running the wired
receiver wrapper resets OTA selection and installs slot 0, while retaining pairing
and diagnostic NVS. An unbootable/unreachable receiver still requires wired
recovery. Protocol-v2 input remains compatible with older boards, but old receiver
firmware cannot provide the new management operations.

## Settings and verification

`config/kms_settings.h` holds management queue/pacing/retry/session timings,
summary interval, event/checkpoint capacities, repeat/checkpoint intervals,
maximum text-transfer size, save coalescing interval and boot-health grace/deadline.
The 256-event/24-checkpoint journal uses schema 2 and starts fresh on the first
update from the old schema; pairing and preferences are preserved. The existing
128 KiB `kms_diag` partition is unchanged, so OTA-capable receivers need only an
application update.

Downloads freeze binary history and identity in a 12,096-byte snapshot (current
layout), then format requested chunks using an index of text-block offsets. The
full formatted text is never allocated on either board. Total length and CRC are
computed over the same frozen data before transfer. Reads can retry or seek
without seeing newly arriving events. The text-transfer limit is 96 KiB, including
the PC helper, browser and master UART command; it is not a receiver RAM buffer.
The ordinary live binary history and a temporary flash-save copy also use RAM.

Updated clients release snapshots with a zero-length read at the exact EOF offset;
older clients can still download and let snapshots expire after inactivity. Old
receivers reject the optional release, which updated clients tolerate. Clear and
expiry also release resources. The master UART command first verifies the snapshot
in chunks, then reads it again for output. Only a verified complete second pass
prints `KMSHISTORY N END`; an interrupted pass reports failure without that marker.

A receiver powered by a USB charger can provide diagnostics and accept OTA while
its normal computer is off. USB-unavailable status is expected without a host;
check USB input delivery later on a running computer.

Run `python scripts/test-core.py` for the production protocol, fake-flash upload
and bounded-history state machines, and `python -m unittest discover -s tests/python -v`
for host transfer/update workflows. The core suite also runs
`python scripts/test-link-receive.py`, which checks the production callback's
rejection of spoofed broadcast OTA/control packets and continued direct traffic,
pairing and channel discovery. Build both production projects. Hardware work still
required:

- Migrate the spare, confirm its pairing survives, enumerate HID and query status.
- Download/clear/reboot history; exercise rollover, grouped repeats and storage failure.
- Update an inactive and selected receiver; repeat with matching and forced images.
- Interrupt radio/serial/power during transfer and first boot; confirm old-image
  recovery and rollback, then test an all-target batch containing an offline slot.
- On a spare, verify unencrypted unicast and broadcast management frames spoofing
  the paired master's MAC cannot start an upload; verify legitimate paired OTA.
- Measure input responsiveness during management traffic and flash writes, and
  check USB suspend/resume, lease releases and prolonged-use storage/heap stability.

Live forwarding of a slave's internal log console remains a future consideration
in [the repository TODO](TODO.md); it is not part of retained-history downloads.
