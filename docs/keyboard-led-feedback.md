# Keyboard lock LED feedback

Merged into `master` by PR 311 and installed on the S3 and all four S2s on
16 October 2026. Pairings and the Insert trigger were retained. The K295 Caps
Lock light, control from both keyboards and opposite-state switching between
two PCs pass. The K830 has no physical Caps Lock indicator. See the complete
record and remaining checks in [testing.md](testing.md).

The selected computer owns the Caps Lock, Num Lock and Scroll Lock state. Its
S2 receives that state through the keyboard HID Output report, sends it over
the encrypted link, and the S3 applies it to every compatible attached keyboard.
Compose/Kana bits are preserved where the PC and keyboard expose them. This is
independent of the S3 RGB status indicator, which stays off normally.

## Behaviour

- Lock keys on either keyboard update the indicators on both, using the PC's
  reported state. A keyboard directly attached to the selected PC can update it too.
- Switching clears the previous computer's LED state. Activation ACKs seed the
  new state; periodic status and change-triggered updates keep it synchronized.
  Feedback from inactive computers, old routes/boots and older LED revisions is
  ignored. Lost updates recover through later status packets.
- With no selected computer, or before its first valid LED report, indicators
  are set off. Lock state is never guessed from local keypresses.
- Reconnected USB keyboards receive the latest selected state. Periodic refresh
  also reapplies it to wireless keyboards waking behind an existing receiver.
  A receiver may acknowledge USB output without delivering it to a sleeping
  keyboard, so actual wake behaviour still needs bench validation.
- USB detach/re-enumeration invalidates the S2's cached state. Suspend retains
  it, while the existing link readiness/reselection policy still applies.

## HID compatibility and USB scheduling

The S3 parses keyboard/keypad Output items separately from Input items, including
report IDs, bit offsets, field widths, padding, multiple output reports and global
push/pop. It emits complete `SET_REPORT(Output)` requests with the correct
interface, report ID and optional ID prefix. It does not whitelist products.

Supported LED reports contain absolute variable Boolean fields for standard LED
usages 1-5 and constant padding. Output arrays, unsupported LED usages and reports
sharing nonconstant vendor/other controls are skipped. The rest of a supported
keyboard's input still works. This avoids filling unknown controls with zero
and changing receiver settings. Some keyboards have no indicator or use
vendor-specific commands; their LEDs cannot be guaranteed.

Transfers use an asynchronous USB host client, sharing each device's existing
endpoint-zero pipe. Only one LED request per physical device is in flight.
Input processing does not wait for completion. Changes arriving during an older
request remain pending; failures get bounded retries and periodic refresh.

The pinned ESP USB library does not implement `transfer.timeout_ms`. The watchdog
logs overdue requests without reusing their DMA buffer, freeing an outstanding
transfer or resetting the shared control endpoint. A request that never completes
stays retained until the stack cancels it on disconnect; reconnecting that input
receiver may be necessary. Retiring interfaces keep device/transfer references
until completion, even after their HID parser has been freed.

S3 `status` includes the selected LED mask and per-interface completion/failure/
pending counters. The S2 copies state under a short critical section; its USB
callback does not send radio traffic.

## Configuration

All options are in `config/kms_settings.h`:

| Setting | Default | Purpose |
| --- | --- | --- |
| `KMS_KEYBOARD_LEDS_ENABLED` | 1 | Enable physical keyboard LED forwarding on the S3 |
| `KMS_HID_LED_FIELDS` | 16 | Maximum parsed LED fields per interface |
| `KMS_LED_RADIO_MIN_MS` | 10 ms | Minimum spacing for change-triggered status packets |
| `KMS_LED_USB_RETRY_MS` | 100 ms | Backoff between failed USB output attempts |
| `KMS_LED_USB_ATTEMPTS` | 3 | Attempts per report per refresh/new state |
| `KMS_LED_USB_WATCHDOG_MS` | 250 ms | Warn about an outstanding asynchronous request |
| `KMS_LED_USB_REFRESH_MS` | 2,000 ms | Reapply state, including after retry exhaustion |

## Firmware compatibility and updates

**Update the S3 and every S2 together during a planned test window.** Current firmware
uses radio protocol **v2 (137 bytes)**; previous firmware uses **v1 (132 bytes)**.
Each rejects the other's packets. Until both ends are updated, that route cannot
forward input or pair. Do not flash just the S3 while relying on installed S2s.

NVS peer-store format, USB IDs/descriptors and trigger preference are unchanged.
Ordinary flashing preserves pairings/trigger; factory reset/re-pairing is not
part of this upgrade. Use the existing [flashing guide](flashing-and-pairing.md)
for future updates. To roll back, rebuild/flash **both roles** from the
previous `master` version; do not erase NVS. All five boards were updated to v2
on 16 October 2026 without clearing pairings or the trigger preference.

## Later hardware validation

- [x] Update the S3 and all four S2s in an agreed maintenance window, preserving NVS.
- [x] Verify Caps Lock text case and the K295's physical indicator through receiver 3.
      The K830 has no physical Caps Lock indicator.
- [x] Confirm Caps Lock from either keyboard updates text case and the K295 light.
- [ ] Test Num/Scroll Lock where keys and indicators exist.
- [x] Set opposite Caps Lock states on two PCs and confirm the indicator follows
      the selected PC when switching between receivers 2 and 3.
- [ ] Change an inactive PC's lock state directly and verify it cannot override
      the selected PC's indicators.
- [ ] Change lock state using a keyboard directly attached to the selected PC.
- [ ] Disconnect/reconnect an input receiver and wake a sleeping keyboard; verify
      state refresh and normal typing/mouse behaviour throughout.
- [ ] Test S2 unplug/replug, suspend/resume and radio loss/reselection.
- [x] Confirm both M220 mouse and K830 touchpad still move/click normally.
- [ ] Retest trigger configuration, lock-key triggers, timeout replay, media keys
      and normal RGB-off behaviour.
- [ ] Check other HID output layouts and receiver models; retain descriptors and
      diagnostics for unsupported cases.

Automated coverage is recorded in [testing.md](testing.md). It does not establish
end-to-end physical LED operation.

## References

- [USB-IF HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf), Output
  items, report ID prefixes and `SET_REPORT` (sections 6.2.2, 7.2.2 and 8).
- Pinned Espressif sources: `usb_host_hid` 1.2.1, `usb` 1.6.0 and `esp_tinyusb`
  2.4.0, declared in the firmware component manifests/lock files.
