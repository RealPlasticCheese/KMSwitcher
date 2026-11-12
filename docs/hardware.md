# Hardware evidence and outstanding checks

The S3 photographs resemble a YD-style 44-pin board, with a UART bridge and a
separate native USB connector. The pictured board revision is not necessarily
identical to every schematic sold under that name.

- The [manufacturer repository](https://github.com/vcc-gnd/YD-ESP32-S3) documents
  BOOT on GPIO0, a WS2812 on GPIO48, and a separate power indicator. It also
  publishes [schematics and board drawings](https://github.com/vcc-gnd/YD-ESP32-S3/tree/main/5-public-YD-ESP32-S3-Hardware%20info).
- A [board owner's investigation](https://github.com/profharris/YD-ESP32-S3_ESP32-S3-WROOM-1_Dev)
  describes clone differences and RGB/USB power solder jumpers. This is a clue
  for inspecting the actual board, not proof that its jumper wiring is identical.
- The ESP32-S2 KEY V1.0 photos show the single button and LED. The vendor's
  screenshot links to a Baidu schematic download, but no accessible schematic
  matching the exact board has established the LED pin. Other MuseLab S2 boards
  and LOLIN S2 Mini pinouts are different hardware.
- Entering the S2 ROM downloader by holding its button is consistent with BOOT
  on GPIO0. Runtime GPIO0 press/release and three-second hold logs confirmed the
  button wiring with the retained board-check diagnostic.
- The S2 LED GPIO needs a schematic or an unpowered continuity check of the LED's series resistor. Use the [sweep diagnostic](../diagnostics/README.md#s2-blue-led-gpio-sweep) on a spare board.

The S3 native USB peripheral is used as host in production, so its native USB
serial console is unavailable then. Use the USB-to-UART connector for production
logs/programming. Diagnostics deliberately use the native USB console instead.
The S2 production firmware exposes three HID interfaces and no CDC serial port;
hold its button while reconnecting to return to the ROM downloader.
A hub is needed for the two input receivers at the same time. Enable hub support
and budget the S3's eight host channels for enumeration, the hub, and HID
endpoints. See [Espressif's host guide](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html).
Confirm the host port's 5 V supply and the hub's upstream power behaviour on the
actual board before attaching receivers. Board connector labels and jumper
arrangements can vary between clones.

No particular Logitech receiver or hub product is whitelisted in the code.
Unsupported vendor-only HID collections are skipped. Standard keyboard, relative
mouse, consumer-control, and system-control usages are parsed from descriptors.
Absolute digitizers, gamepads, proprietary gestures, and device configuration
protocols are outside the current input implementation.

## HID compatibility and simultaneous input capacity

Input receivers do not need enrolling with the S3. They must already be paired
with their own keyboard/mouse, then expose supported standard USB HID reports.
The S3 reads each HID report descriptor, maps its fields to keyboard, relative
mouse, consumer and system controls, and merges supported input into the selected
S2's output. There is no receiver model whitelist. Proprietary reports requiring
a vendor driver, generic Bluetooth adapters, and unsupported HID descriptor forms
are not automatically supported.

Automatic input selection activates at most two physical input devices and
keeps each device's supported HID input interfaces together within the channel
budget. This is generic descriptor-based selection, with no exceptions for the tested receiver
models. See [USB discovery and selection](usb-input-selection.md) for automatic
and manual choices, persistence and missing-device recovery.

The ESP32-S3 has eight hardware USB host channels, and the current Espressif stack
allocates a channel to each device's default control endpoint and each claimed
data endpoint. The hub consumes channels too. See
[Espressif's host-channel limits](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#host-channels).

The already tested K295/M220 and K830 interfaces account for this budget:

| Attached component | Channels in the current stack |
| --- | --- |
| One hub: control endpoint plus status interrupt endpoint | 2 |
| K295/M220 receiver: control plus keyboard and pointer input endpoints | 3 |
| K830 receiver: control plus keyboard and pointer input endpoints | 3 |
| Total | 8 |

With those interfaces open, another receiver has no free channel even for its
initial control endpoint. Vendor-only interfaces are skipped, but a receiver's
keyboard/pointer interfaces still count when only a mouse is paired to it if
those interfaces are exposed and opened. Simpler receivers can use fewer
channels, so channel capacity remains independent of the two-input policy.
The selector ignores unsupported/vendor-only input, but keeps each selected
device's supported inputs together: an unpolled keyboard can otherwise stall a
mouse sharing its receiver. Complete bundles may leave fewer than two active
devices in a crowded hub chain. Hub and device-control costs remain.

`KMS_USB_HID_INTERFACES` and `KMS_INPUTS` limit software interface/report tracking;
raising them or adding hub ports does not increase hardware channel capacity.
More simultaneous input receivers would need channel sharing in a different
host implementation or additional host hardware. The seven paired S2 computer
destinations use ESP-NOW and are independent of this USB input-side limit.
