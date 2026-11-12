# KMSwitcher requirements and design notes

Updated 16 October 2026. The switcher implementation is present and both
chip builds pass. These requirements define intended behaviour; compilation and
core tests do not establish end-to-end hardware operation. See
[the verification record](testing.md) for tested and outstanding items.

## Agreed behaviour

The desktop-assisted extension is specified in
[screen-edge switching](screen-edge-switching.md): dynamic monitor boundaries,
four-direction station layout, automatic skipping, dock identity discovery,
Windows/Linux helpers, tray controls and CLI configuration through any S2.

- One ESP32-S3 acts as master and USB host for keyboards, mice, and wireless
  keyboard/mouse receivers.
- Multiple input receivers operate simultaneously through a USB hub. All input
  goes to the same selected computer; switching from either keyboard changes
  the shared destination.
- Support up to seven paired ESP32-S2 receivers, assigned directly to computer
  slots 1-7. The user currently owns four S2 receivers. Retain the default
  seven-peer encryption capacity.
- Every S2 stays enumerated as USB HID on its own computer. All powered paired
  S2s remain available over the radio, including inactive destinations.
- Switching changes input routing without USB re-enumeration or radio pairing.
- The S2 receivers use the same firmware. Slot identity is persistent pairing
  configuration, not a separate firmware build per computer.

## Switching shortcut

- Two complete standalone taps of the same configured trigger enter selection mode.
  Ctrl is the default. For Ctrl/Alt/Shift, either side works but both taps must
  use the same physical key; the default double-tap window is 350 ms.
- Detect the taps per input keyboard. A tap on each of two keyboards must not
  accidentally form the shortcut.
- Selection mode lasts three seconds and retains the two trigger taps.
- Number keys 1-7 select the correspondingly numbered receiver. The successful
  shortcut and selection digit are consumed rather than sent to a computer.
- If the timer expires without selection, replay both trigger taps to the current
  computer.
- Mouse input continues to the current computer during selection mode.
- Another keyboard key cancels selection and forwards the retained trigger taps,
  followed by that key, to the current computer, except reserved P/C commands.
- Escape cancels selection and discards the retained shortcut.
- Ordinary Ctrl combinations, including Ctrl+C and Ctrl+mouse operations, must
  preserve their normal meaning and event order.
- Double-tap the current trigger, C, then press/release Ctrl, Alt, Shift,
  Scroll Lock, Insert, Pause/Break or Num Lock to persist a new global trigger.
  Escape or the ten-second timeout cancels without replay or command leakage.
- Three short S3 BOOT presses within two seconds restore the default trigger,
  retaining pairings. A five-second BOOT hold started during the ten-second
  normal-startup window, followed by release, clears S3 settings and pairings.
  Do not hold BOOT during power-on/reset, which enters the ROM downloader.
- Normal S3 RGB indication defaults to off. Pairing and trigger recovery use
  blue feedback; colours, patterns and timings live in `kms_settings.h`.

Implementation defaults: accept top-row and numeric-keypad keys 1-7 in
selection mode. Selecting an unpaired or unavailable slot retains the current
destination and indicates failure; command keys must not leak into applications.

## Generic USB HID support

Compatibility is based on USB HID report descriptors and usages. Do not restrict
runtime support to Logitech or whitelist particular product IDs or hub models.

The initial supported functions should include:

- Keyboard keys and modifiers, including boot and report-protocol layouts.
- Mouse/touchpad pointing, buttons, and scrolling as exposed through mouse HID.
- Standard Consumer Control usages, such as volume, mute, and media playback.
- Standard system-control usages where supported by the output HID descriptor.
- Physical keyboard lock indicators driven by the selected PC's HID LED output,
  synchronized across compatible keyboards on selection, reconnect and refresh.
  Skip output reports containing unsupported/vendor controls. See the
  [implementation and deferred validation plan](keyboard-led-feedback.md).

Report IDs, bit offsets, field widths, and collections can differ even when
devices implement the same standard usages. Parse these descriptors rather than
assuming fixed Logitech packet layouts or supporting only the boot protocol.
Maintain input state per device and merge held keys/buttons so one device's
release does not clear another device's held input. Handle removal of an input
device by clearing only its contributions.

Manufacturer-specific configuration, receiver re-pairing, and proprietary
touchpad gestures require separate consideration. Generic HID support does not
imply arbitrary vendor-specific feature forwarding. Bound parser and report
sizes and reject unsupported descriptors safely.

## Devices inspected on Windows

Read-only Windows PnP enumeration confirmed the following connected receivers.
No live keystrokes were captured and no device settings were changed.

| User's input devices | USB ID | Observed USB interfaces | Observed HID functions |
| --- | --- | --- | --- |
| Logitech K295 / M220 desk set | 046D:C534 | MI_00 and MI_01 | Keyboard, mouse, consumer control, system control, vendor-defined collections |
| Logitech K830 via Unifying receiver | 046D:C52B | MI_00, MI_01, and MI_02 | Keyboard, mouse, consumer control, system control, vendor-defined collections |

These are test devices, not a runtime compatibility allowlist. PnP enumeration
establishes advertised functions; it does not validate individual media keys,
raw report layouts, endpoint requirements, or operation through the S3 host.

## Radio and switching reliability

Use ESP-NOW with persistent peers on a common 2.4 GHz channel, unicast input to
the active receiver, and lightweight health/status traffic to inactive peers.
Keep radios listening for responsive switching; no router is required.

Retain the installed ESP-IDF v6.1 default of seven encrypted peers on the master:
`CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM=7` when implementing the radio component.
Each S2 requires one encrypted peer for the master. Validate actual peer
registration and encrypted operation with seven devices as hardware becomes
available. The legacy `ESP_NOW_MAX_ENCRYPT_PEER_NUM` header constant is six;
use the configurable Wi-Fi limit and runtime validation when implementing
capacity checks, rather than treating that old constant as the application limit.

Use per-peer encryption keys, application acknowledgements for state changes,
sequence numbers, duplicate rejection, and a session/routing generation to keep
late packets from a previous destination or session from generating input.
Recover the selected receiver automatically after link errors. Buffer ordered
input in RAM for a configurable short window (five seconds by default), with a
fixed capacity, and discard stale/overflowed input rather than replaying it late.
Explicit switching or pairing discards the previous destination's backlog.
Switching and receiver fail-safe expiry must clear held output keys/buttons. A powered-off
or disconnected receiver must not block input routing indefinitely. Measure
end-to-end latency and loss recovery; no latency guarantee has been established.

## Pairing

Double-tap the current trigger, P, then a number from 1-7 opens a 30-second pairing
window for that computer slot on the master. A long press of the S2's button while its firmware is running
requests enrollment. Save the slot, master identity, and peer keys across power
cycles, and provide success/failure feedback.

Button wiring is confirmed on GPIO0; software control of the S2 LED still
requires confirmation.
Do not assume a power indicator is programmable. Use the S3's RGB indicator for
feedback if the S2 has no controllable LED. Pairing pauses input routing, flashes
the S3 RGB LED blue and requires explicit destination selection afterward.

## USB hub and power

The powered test hub works with both original receivers. Target
standards-compliant hubs with at least two downstream data ports, without
selecting a specific brand/model. Verify
upstream full-speed operation and the attached HID devices' speeds. Check the
S3 board's host VBUS supply and the hub's power arrangement before connecting
the hardware; a USB-C adapter alone does not establish host power capability.

The S3 has one native USB-OTG host, with eight host channels. Its two USB-C
connectors do not provide two independent native host buses. Enable the USB
host hub component and claim only required interfaces/endpoints, budgeting for
the hub and enumeration. Use the board's USB-to-UART bridge for programming and
logs once native USB is used as host. The tested hub and two combo receivers
consume all eight channels in the current stack; other combinations need testing.

## References

- [USB-IF HID specifications and usage tables](https://www.usb.org/hid)
- [Espressif USB HID host driver](https://components.espressif.com/components/espressif/usb_host_hid)
- [Espressif USB host guide and channel limits](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html)
- [ESP-NOW configuration and limits](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_now.html)
