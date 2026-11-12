# USB input discovery and selection

The S3 discovers enumerated USB devices and reads their standard HID report
descriptors before claiming input endpoints. Automatic selection activates
**at most two physical input devices**, subject to the ESP32-S3's eight host
channels. A shared keyboard/mouse dongle counts as one device. Hubs do not count
toward the two-input limit, but consume channels.

Each selected device's supported HID input interfaces stay together. An unpolled
interface can block another function sharing the device's internal queues: the
live test reproduced a receiver's mouse freezing when its standby keyboard was
used. Keeping complete supported input bundles avoids relying on partial-device
behaviour. Unsupported/vendor-only reports and non-HID interfaces are not
claimed as input. This is a generic rule, with no brand/model or VID/PID whitelist.

Automatic selection favours keyboard/mouse coverage, then two complete input
devices when they fit. Explicit choices take priority; ties favour active
devices and fewer channels. It cannot promise two simultaneous inputs when their
complete supported interfaces exceed the available budget.

## Choose inputs

In [Companion](installing.md#device-management-in-companion), use **Devices → USB inputs → Refresh USB inputs**.
Each HID interface shows its functions, endpoint cost, state and decoded/report
counters. **Allow device** includes or excludes the entire physical input device;
its other rows say **Same device**. **Active** confirms selection. An allowed
**standby** device's complete supported inputs did not fit or another device had
priority. **Unsupported** and **error** identify descriptor/transfer problems.

Automatic mode starts with supported devices eligible. Manual mode keeps
currently active devices and defaults new ones to off. Changing mode clears
previous overrides. Manual choices still respect the two-device/channel limits.
Refresh after a change to check activation; discovery and transfer cancellation
run asynchronously. Deselecting/removing input releases its held keys/buttons.

Choices are saved asynchronously on the master using VID, PID, serial-number
hash (when supplied) and interface identities, rather than temporary USB addresses.
A device action updates all its HID interface rules together. Reconnects and
restarts retain choices; identical devices without serial numbers share them.
Up to 24 interface rules are retained. Changing selection mode clears overrides;
the physical factory reset restores automatic selection. Preference-only reset
does not change USB choices. `KMSUSB SAVED` confirms persistence; a save error
leaves choices effective for the current boot.

Console equivalents:

```text
usb-list
usb-input ADDRESS INTERFACE on
usb-input ADDRESS INTERFACE off
usb-auto on
usb-auto off
usb-rescan
usb-recover
```

Use a current address/interface from `usb-list` to identify its physical device;
`usb-input` changes the whole device. `usb-auto on` restores a clean automatic
policy. `usb-rescan` retries enumerated inactive HID descriptors without a restart
or pairing reset. Inventory printing and NVS saving use low-priority workers.

`usb-recover` performs a full logical USB reconnect, releases stale input and
rediscovers devices while retaining the selected PC and saved input choices.
The master also does this automatically when control transfers or health probes
stop completing. See [frozen USB recovery](recovery-and-diagnostics.md#frozen-usb-input-recovery)
for thresholds, restart limits and the distinction from switching hub DC power.

## Understand the channel budget

The budget is device controls + hub status endpoints + claimed input endpoints.
One hub uses two channels. Two combo receivers with a control and two input
endpoints each use three apiece: eight in total. Both complete combos fit.

A keyboard containing another hub adds that hub's control/status cost. In the
tested chained setup, four controls and two hub status endpoints leave only two
input channels. The complete combo needs two, and the wired keyboard needs two
more (keyboard plus media/system). They cannot both be active. Even a hypothetical
keyboard-only interface plus the complete combo would need nine channels. The
selector keeps the combo complete rather than sacrificing its keyboard to fit
the wired keyboard alongside its mouse.

Ignoring unsupported/vendor-only interfaces saves their claimed endpoint cost,
but an enumerated physical device's control endpoint still occupies a channel.
Every endpoint of a claimed interface counts, including interrupt OUT if present.
Lock-light control transfers use the existing control endpoint.

With eight channels occupied, another device may fail to enumerate and cannot
appear in the inventory. Disable an input device to free endpoint channels, then
physically reconnect the missing device. A descriptor retry cannot recover a
device that never enumerated. Selection does not add or time-share hardware
channels. See [Espressif's channel allocation description](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html#host-channels).

Discovery tracks up to eight physical devices (including hubs) and 24 non-hub
interfaces. The SDK suppresses hub events to application clients, so discovery
uses the public address list and caches hub metadata without pinning lifetimes.
Existing descriptor/parser/report-source limits still apply. A device must
enumerate and expose supported standard HID descriptors; arbitrary USB HID
compatibility is not promised.

## Validation and remaining compatibility

Native tests cover complete generic combos, tight budgets, explicit choices,
and 1,000 varied candidate sets with no partial input bundles. Fake asynchronous
host tests exercise discovery before claims, hidden hubs, cancellation/unplug,
short/error transfers, reconnect identities, late channel availability and the
two-device cap. Browser tests validate inventories and whole-device controls.

The corrected S3 build passes the normal one-hub/two-combo live test: both
keyboards, the Logitech mouse after typing and the HTPC touchpad work. The old
wired keyboard also decoded reports and worked on the PC during its test. The earlier partial receiver selection froze its mouse when its keyboard
was used, motivating the complete-input rule. The old numeric keypad still has
USB enumeration/recovery errors despite working on a PC; it is not a verified
compatibility fix. See the [live test record](testing.md).
