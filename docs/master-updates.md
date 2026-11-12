# Master firmware updates

The browser can update the S3 application through its existing COM/UART bridge.
It uses a local `master HEX` endpoint, not the radio path or a ROM flashing tool.
The same uploader/verification code serves S3 master and S2 receiver updates.
For receiver command-line instructions, see
[Updating one or all receivers](slave-management.md#updating-one-or-all-receivers).

## One-time wired migration

The previous S3 layout has one factory application and no OTA slots. Install this
change once using the normal wired wrapper; subsequent application updates can
use the browser. Do not attempt to send only this application to the old firmware.

```powershell
.\scripts\idf.ps1 -Project controller -ResetConfig
.\flash-controller.cmd COM8
```

Identify the actual UART COM port first. The generated flash arguments include
the rollback-capable bootloader, new partition table, factory application and
initial OTA selection data. Pairing/preferences/history NVS remains at `0x9000`
for `0x6000` bytes; PHY remains at `0xf000`. Do not erase flash or reuse old offsets.
The existing saved trigger is imported when the new preference record is absent.

The 16 MiB S3 retains the factory application at `0x10000`, size `0x300000`.
OTA selection data starts at `0x310000` (8 KiB), followed by 3 MiB application slots
at `0x320000` and `0x620000`. The untouched factory application provides a fallback
for the first OTA boot; later updates alternate between the two OTA slots.

Wired recovery remains available through the normal wrapper. It resets OTA
selection and reinstalls the factory application without erasing NVS. A receiver
update is unnecessary for master OTA or everyday preferences; older protocol-v2
receivers can keep forwarding input, with receiver management capabilities as
provided by their installed firmware.

## Expanded history layout

The 1,024-event/72-checkpoint firmware adds a 256 KiB `kms_diag` NVS partition at
`0x920000`, after the existing OTA slots. All existing offsets remain unchanged.
Install the updated partition table and application once through the wired UART
wrapper (the build and flash commands above); do not send only this application
through OTA to a board without `kms_diag`. Do not erase the whole flash.

This update starts a new diagnostic history. Existing pairing, everyday preferences
and USB selections remain in the original NVS partition; no history migration is
performed. Once the table is installed, later application updates can use OTA.
Check `status` for `History=1024 events/72 checkpoints`, heap headroom and zero
storage errors. Verify all paired slots and the saved trigger/boot destination.

## Receiver firmware bundled with the master

The controller build first builds the receiver from the same source revision. It
produces `receiver-cache.bin` for initial installation and embeds the exact S2
binary and manifest in `kmswitcher_controller.json`. Keep the controller `.bin`
and its complete JSON together. The JSON is larger because it carries the S2
payload; the controller application itself remains subject to its 3 MiB limit.

Install the expanded partition table once with `flash-controller.cmd`. Two 2 MiB
data partitions follow diagnostic storage: `s2_cache_0` at `0x960000` and
`s2_cache_1` at `0xb60000`. Existing app, pairing, preference and diagnostic
offsets remain unchanged. The layout leaves 2.625 MiB unallocated. The wired
installer preloads slot 0 and invalidates slot 1's header. It preserves pairing,
preferences and history, but replaces the cached receiver firmware.

Later master updates through the browser or Firmware Setup first confirm the
S3 application, then refresh its S2 cache from the bundled JSON if the cache hash
differs. A skipped matching master application can still refresh its cache. The
application and cache are separate transactions: a cache failure is reported
separately and can be retried without reinstalling the S3 application. An old
partition table requires the one-time wired installation before caching works.

Cache replacement writes the inactive slot, validates the S2 chip/project, exact
SHA-256 and appended image digest, then commits its header. Interrupted uploads
leave the previous verified slot available. Startup checks both slots and falls
back if the newest is corrupt. Normal cache uploads do not restart the S3 or
automatically install anything on a receiver. An installation or cache upload
excludes another cache transfer and master application upload.

See [receiver cache commands](slave-management.md#firmware-stored-on-the-s3) for
standalone replacement and installation on selected dongles. Physical power-loss,
timing and input-responsiveness checks still need device testing.

## Browser workflow

1. Connect the master UART and click **Inspect master** to check identity/OTA support.
2. Select `kmswitcher_controller.bin` and its matching generated JSON manifest,
   or choose **Master controller** as the update target. Master images must have
   controller role, ESP32-S3 chip ID 9, management v1/input v2, OTA schema 1, and fit
   the 3 MiB slot. Receiver images cannot be sent to the master or vice versa.
3. Review the confirmation. Matching image hashes are skipped; forced reinstalls
   still check both hashes and require a different confirmed boot ID.
4. Keep the tab/USB cable connected. Input pauses before writing. Ordered,
   CRC-protected chunks go to the inactive slot, followed by flash-readback
   SHA-256, ESP image and project-name checks before activation. Transfer and
   verification run at the console speed you selected, including 4 Mbaud.
5. The S3 restarts while its UART bridge stays enumerated. The browser keeps the
   selected speed through activation, then reopens at 115200 (the startup rate)
   and requires a new boot reporting the expected confirmed image. A missing
   activation acknowledgement does not repeatedly retry at the old speed. After
   confirmation the browser restores the selected console speed.
   It then attempts to restore the exact previous destination, or keeps input
   paused if it was already paused. Reload preferences before editing after a
   written update, because their revision changes with the master boot.

Master update mode blocks new selection, pairing and preference writes, including
physical shortcut changes, while the upload is active. A pending OTA boot also
keeps routing paused. The new image confirms only after its application loop,
USB-host initialization, valid build identity and local management probe pass the
startup grace period. No attached input device or online receiver is required.
Failure to confirm by the deadline triggers rollback; a worker startup failure
rolls back immediately. Factory/normal confirmed boots retain normal startup
selection behaviour. These health checks do not prove end-to-end input delivery.

Incomplete/invalid uploads never activate. Expired sessions are abandoned. A
verification failure before activation attempts abort; the browser also attempts
route restoration if pausing was requested. If activation or the serial connection
is interrupted, inspect the actual running image before retrying. Download the
JSON result report: transfer/rollback errors and destination-restoration warnings
are retained separately. If the browser closes after confirmation but before
restoring input, use a normal selection shortcut or console `select N`.

## Local endpoint

`master HEX` accepts a management-v1 envelope and returns `KMSMASTER HEX`.
Its envelope slot is 1 solely for the existing codec; the command is handled
locally and never addressed to receiver 1 or transmitted over ESP-NOW. Browser
code represents this local target as 0, separate from radio slots 1–7. Identity
and OTA operations use the same request/boot IDs, upload tokens, CRC and exact
image hashes as receiver transfers. Radio protocol formats remain unchanged.

Physical migration, skip/force updates, interrupted serial/power, rollback,
first-boot routing, preference retention and wired recovery remain hardware
validation tasks in [TODO](TODO.md).
