# Configuration

Firmware defaults and engineering limits live under [`config/`](../config).
Everyday user preferences are saved on the S3 and can be changed from
[Companion → Devices → Preferences](installing.md#device-management-in-companion) or the UART console protocol. C source contains HID/USB
usage codes and protocol layout constants; these are defined by specifications,
not board tuning values.

| File | Change here |
| --- | --- |
| `config/kms_settings.h` | Default trigger, shortcut/configuration/recovery/reset timing, radio settings, queue/parser capacities, RGB enable/colours/patterns/brightness, USB identity and polling |
| `config/board_profiles.h` | BOOT button pin/polarity, S3 RGB data pin/model/colour order, optional S2 LED pin/polarity |
| `config/sdkconfig.common` | ESP-IDF settings shared by both production roles, including encrypted peer capacity, tick rate, main task stack, and cryptography |
| `config/sdkconfig.flash.esp32s2` / `.esp32s3` | Flash capacity, mode and frequency for every role on that chip |
| `config/sdkconfig.controller` | S3 UART console, USB hub support, PSRAM log storage and partition layout selection |
| `config/sdkconfig.receiver` | S2 USB HID interface count, UART console, OTA layout and boot rollback |
| `config/partitions.controller.csv` / `partitions.receiver.csv` | Production flash layouts preserving pairing NVS; receiver dual OTA slots and diagnostic NVS |
| `config/sdkconfig.diagnostic.esp32s2` / `.esp32s3` | Board-check native USB logging and self-test settings |
| `config/led_sweep.h` | S2 LED diagnostic candidate pins, dwell time, repeats and gaps |
| `config/sdkconfig.led-sweep.esp32s2` | LED sweep TinyUSB logging, disabled UART and manual no-reset flashing |
| `config/sdkconfig.bringup.esp32s2` / `.esp32s3` | Retained original bring-up console settings |
| `toolchain.json` and component manifests | Pinned SDK, installation manager, and component versions |

Names include units: `*_MS` is milliseconds, `*_BYTES` is bytes, and
`*_HZ` is hertz. GPIO `-1` means the S2 LED is disabled. Do not replace it with
a pin from a different S2 board: the KEY V1.0 LED wiring is still unverified.
An S3 RGB GPIO of `-1` disables that LED as well.

Management pacing/retry/session limits, slave history capacities/checkpointing,
snapshot buffer size and OTA health timings also live in `kms_settings.h`.
See [slave diagnostics and updates](slave-management.md) for their behavior and
the one-time wired migration. Regenerate existing production `sdkconfig` files
with `-ResetConfig` before migrating to the new partition layouts.

## Everyday preferences

The S3 stores a versioned, CRC-checked preference record in its `kmswitcher` NVS
namespace. First use imports an existing saved trigger; missing/invalid records
fall back to the header defaults plus a valid legacy trigger. Mouse jiggling
requires compatible S3 and S2 firmware; the other settings only require the S3:

| Preference | Default | Accepted values |
| --- | --- | --- |
| Trigger key | Ctrl | Ctrl, Alt, Shift, Scroll Lock, Insert, Pause/Break, Num Lock |
| Double-tap window | 350 ms | 100–1,000 ms |
| Destination-selection timeout | 3,000 ms | 500–15,000 ms |
| Mouse jiggler interval | 30 seconds | Whole seconds, 1–65,535 |
| Mouse jiggler distance | 1 count | Relative HID counts, 1–127 |
| Normal RGB indication | Off | Off/on |
| RGB brightness | 8 | 1–255; use normal-indication off to hide normal status |
| Per-slot colours | Header palette | Off, red, green, yellow, blue, magenta, cyan, white |
| Keyboard lock-LED feedback | On | Off/on |

Load values before editing, then Save to validate, persist and apply them. Saves
release/block held input and reset an in-progress shortcut rather than carrying
old timing/key state into the new settings. A save error leaves the running
preferences unchanged. Normal indication off does not suppress temporary pairing,
selection, recovery, startup or reset feedback. Brightness applies to the RGB
indicator; it does not change physical power LEDs or keyboard light intensity.
Disabling keyboard LED feedback stops new output reports while still processing
in-flight completions/removal; enabling it refreshes attached keyboards without
USB re-enumeration.

### Mouse jiggler

While jiggle is enabled for the selected dongle, the controller LED briefly turns
off for 150 ms every five seconds. If normal indication is disabled or that
dongle's colour is off, it flashes blue instead. Other status indications take
priority. Switching dongles shows the newly selected dongle's jiggle state.

Select a computer, double-tap your configured shortcut trigger, then press **J**
within the configured command window to toggle that S2's jiggler. Repeat to turn
it off. For example, use Insert, Insert, J when Insert is configured, or Ctrl,
Ctrl, J when Ctrl is configured. This uses the same trigger as computer selection.
The shortcut is consumed locally and does not type the trigger or J on the computer.

Double-tap the same trigger, then press **K** to cycle the selected dongle's mode:
**Left/right → Four-way → Random → Left/right**. Its on/off state is preserved,
so choosing a mode while disabled does not enable it. The mode is independent for
each dongle. Ordinary K typing outside the shortcut is unchanged.

| Mode | Movement after the idle wait |
| --- | --- |
| Left/right (default) | Left, right, repeating; one step per configured interval |
| Four-way | Up, down, right, left, repeating; one step per configured interval |
| Random | Continuous strokes in horizontal, vertical or diagonal directions, about 50 steps/second; direction changes roughly every 0.2–1.1 seconds |

All modes use the configured distance for each step. Host pointer settings
determine the resulting screen pixels. Each S2 runs independently,
including while another dongle is selected, the S3 route is paused, or radio
contact is lost. Movement preserves any held mouse buttons and keyboard state.
Real mouse movement or scrolling pauses jiggling and restarts the wait. Once
there has been no movement for the configured interval, the chosen mode resumes.
Random mode runs continuously after that wait; the other modes wait one interval
between steps. Keyboard input and unchanged mouse snapshots do not
restart the wait. Real movement and jiggles are sent separately.
An unavailable USB host pauses delivery without accumulating a movement burst.

Change **Mouse jiggler interval (seconds)** in the configuration tool, or enter
`jiggler-interval 10` in the S3 console for a 10-second interval. Enter
`jiggler-interval` to read the saved interval. This S3 preference survives its
restart and updates all reachable compatible S2s, including inactive dongles;
unreachable receivers adopt it on reconnect. Interval changes restart the wait
without changing whether a dongle is enabled. `status` lists each compatible
dongle's state, mode and any request awaiting confirmation. Interval and distance
are shared S3 preferences; changing them preserves each dongle's selected mode.

**Mouse jiggler distance (counts)** sets the movement size from 1 to 127 relative
HID counts; the default is 1. The console command is `jiggler-distance 4`, or
`jiggler-distance` to read it. Increase it if a one-count step is invisible after
the PC's pointer processing. A mouse count does not guarantee one screen pixel.
Distance is saved on the S3 and applied to reachable upgraded S2s, preserving
their enabled state. A distance change restarts the idle wait. Older S2 firmware
keeps its one-count behavior; enabling it with a larger configured distance is
rejected with an upgrade message. It can still be turned off.

In **Companion → Devices → Dongles**, choose a dongle and **Read jiggler**.
Use **Turn on jiggler**, **Turn off jiggler**, or choose a movement mode.
These controls work with compatible reachable receivers without changing the
active input route. The companion shows when the dongle confirms a change.
The console equivalents are `jiggler get ID N` and `jiggler set ID N on|off`,
where N is the paired slot and ID is a request number echoed in the reply.
Use `jiggler-mode N` to query a dongle or `jiggler-mode N left-right`,
`jiggler-mode N four-way`, or `jiggler-mode N random` to choose its mode.
The correlated UART command is `jiggler mode ID N MODE`.
Older S2 firmware keeps ordinary left/right on/off control where supported;
mode selection is unavailable until the receiver is upgraded.

The jiggler pauses while USB is suspended or disconnected. With an updated
companion and receiver it also pauses when the OS announces sleep/hibernate or
shutdown, preserving its enabled setting. See [computer power reporting](computer-power.md).

Updated S2 firmware saves enabled state, mode, interval and distance in its own
`kmswitcher` NVS namespace under `jiggler`. These settings survive S2 restarts
and firmware updates; saved enabled state can resume without the S3 radio link.
Movement progress and command session/sequence remain in RAM: a restart begins
a fresh interval and movement pattern. The S3 adopts the restored enabled state
and mode, while its shared interval/distance preferences still apply on reconnect.

Only actual configuration changes request a background write; repeated status,
retries and jiggles do not wear flash. Remote restart flushes pending settings
before arming its reboot delay. Storage failures are logged as `KMSJIGGLER`,
leave the current setting active in RAM and retry after one second. An abrupt
power loss before the background commit can lose the latest change. Invalid or
missing records start disabled with the default interval/distance; they never
erase pairing records. A deliberate S2 factory reset clears its saved jiggler.
Older S2 firmware retains its RAM-only behaviour until updated.

The preference codec remains 32 bytes. Version 4 uses byte 27 for distance and
retains bytes 10–11 for the unsigned little-endian interval. Versions 1–3 migrate
to a one-count distance; versions 1 and 2 also adopt the 30-second interval.
Existing shortcut, colour, boot and timing settings are preserved.

**Restore defaults** resets only these preferences. It keeps paired computers,
link keys and diagnostics. Triple-BOOT recovery still resets only the trigger;
the deliberate hardware factory reset still clears the whole `kmswitcher`
namespace, including pairings and preferences. Changing a compiled default does
not override an already saved preference.

### Console protocol

`prefs-get ID` returns `KMSPREFS ID OK REV HEX`, containing the current revision
and the 32-byte canonical preference record. `prefs-set ID REV HEX` writes a
complete validated record. `prefs-default ID REV` restores compiled defaults.
Responses use `OK`, `CONFLICT`, `INVALID`, `STORAGE` or `BUSY`; they include the
current revision and values. Reload after a conflict rather than overwriting a
physical trigger change. Revisions are seeded per boot, so a stale page must also
reload after a controller restart. Preference changes are rejected during master
OTA/first-boot confirmation. This protocol is local to the S3 UART and does not
change the radio input protocol.

## Changing firmware defaults and limits

1. Edit the appropriate versioned file.
2. Rebuild the affected role with `scripts/idf.ps1 -Project controller` or
   `-Project receiver`. Header changes are picked up automatically.
3. After editing an SDK defaults file, add `-ResetConfig` to regenerate the
   ignored `sdkconfig`. This discards local `menuconfig` overrides. Commit lasting
   SDK changes to the defaults files so another checkout reproduces them.
4. Flash each affected board. Country changes require rebuilding both roles.
   The radio channel is a saved runtime setting; see [radio channels](radio-channels.md).
5. Run `python scripts/test-core.py` after changing shortcut or parser settings.
   The current scenarios test seven destinations, LED encoding, route validation
   and a fake asynchronous USB host.

Both chip builds must retain the same base wire format. `kms_protocol.h` contains
the version, state dimensions, and standard HID usage constants. Changing that
layout requires a protocol version change, updated codecs/USB descriptors, and
updating every board. Optional extensions carry their own version and require
explicit receiver capability negotiation before transmission; ordered input
batches retain the existing v2 format for older peers. Increasing `KMS_SLOTS`
beyond seven is intentionally rejected.
The USB interface count must match the three interfaces in the receiver descriptor.

Keep the snapshot interval below the receiver lease. Retry deadlines and input
age limits must remain shorter than the lease. Longer queues consume RAM and can
hide a slow consumer; they do not increase USB hardware endpoint capacity.
Clock comparisons support 32-bit millisecond wrap; timing intervals must remain
below half that clock range (about 24 days).
Parser limits bound memory use and the descriptors accepted by the S3.

USB VID/PID values are development identifiers and do not establish an allocation
for distributing hardware. Each S2's USB serial string is derived from its own MAC.
Slot assignments and generated per-peer encryption keys live in NVS, not in the
source, and survive ordinary application firmware updates. No Wi-Fi router or
credentials are required.

## Trigger, buttons and RGB defaults

The trigger can be changed without rebuilding: double-tap the current trigger,
press **C**, then press and release a supported new trigger. See the
[operation and recovery instructions](flashing-and-pairing.md#change-the-double-tap-key).
This preference is stored on the S3 and applies to every input keyboard.

| Setting | Default / purpose |
| --- | --- |
| `KMS_TRIGGER_DEFAULT` | `KMS_TRIGGER_CTRL`; fallback on first use and trigger recovery |
| `KMS_DOUBLE_MS` / `KMS_SELECT_MS` | 350 ms double-tap window / 3,000 ms command window |
| `KMS_CONFIG_MS` | 10,000 ms to press and release a new trigger after C |
| `KMS_BUTTON_RECOVERY_PRESSES` / `KMS_BUTTON_MULTI_PRESS_MS` | Three short BOOT presses within 2,000 ms restore only the default trigger |
| `KMS_BUTTON_SHORT_PRESS_MS` / `KMS_BUTTON_DEBOUNCE_MS` | Short presses under 500 ms; debounce 30 ms |
| `KMS_FACTORY_RESET_WINDOW_MS` / `KMS_FACTORY_RESET_HOLD_MS` | Start a BOOT hold in the first 10,000 ms; hold 5,000 ms, then release to reset |
| `KMS_BUTTON_HOLD_MS` | 3,000 ms: S2 pairing; S3 status outside its startup reset window |
| `KMS_LED_NORMAL_ENABLED` | **0**, so the S3 RGB LED is off during normal forwarding; 1 restores selected-slot colours |
| `KMS_STATUS_LED_BRIGHTNESS` | 8 per active RGB channel (0-255) |
| `KMS_LED_PAIRING_COLOR` / `KMS_LED_PAIRING_BLINK_MS` | Blue, toggles every 250 ms |
| `KMS_LED_RECOVERY_COLOR` / `KMS_LED_RECOVERY_FLASHES` | Blue, three flashes after restoring the trigger |
| `KMS_LED_CONFIG_COLOR` / `KMS_LED_CONFIG_BLINK_MS` | Yellow, toggles every 250 ms while configuring |
| `KMS_LED_CONFIG_SAVED_COLOR` / `KMS_LED_CONFIG_SAVED_FLASHES` | Blue, two flashes on save |
| `KMS_LED_CONFIG_CANCEL_COLOR` / `KMS_LED_CONFIG_CANCEL_FLASHES` | Yellow, one flash on cancellation |
| `KMS_LED_NOTICE_ON_MS` / `KMS_LED_NOTICE_OFF_MS` | 150 ms on / 150 ms off per confirmation flash |
| `KMS_LED_STARTUP_COLOR` / `KMS_LED_STARTUP_MS` | White for 500 ms when controller controls become ready |
| `KMS_LED_FACTORY_HOLD_COLOR` / `KMS_LED_FACTORY_READY_COLOR` | Blinking red during startup hold; solid red when ready to release |
| `KMS_LED_FACTORY_BLINK_MS` | 150 ms per startup-hold toggle |
| `KMS_LED_SELECTION_COLOR` / `KMS_LED_SELECTION_BLINK_MS` | Yellow, toggles every 250 ms during selection |
| `KMS_LED_FAILURE_COLOR` / `KMS_LED_FAILURE_BLINK_MS` | Red, toggles every 125 ms for failure indication (twice the normal blink speed) |
| `KMS_LED_FAILURE_FLASHES` / `KMS_LED_ERROR_MS` | Five failure flashes; 1,250 ms total, including the final off interval |

Other indicator settings include `KMS_LED_SLOT_COLORS`, `KMS_LED_IDLE_COLOR`,
`KMS_LED_SAVE_ERROR_FLASHES`. Colour masks use the named
`KMS_COLOR_*` constants and are independent of the board's RGB wire order.
Normal-off does not suppress temporary startup, selection, pairing,
configuration, recovery, factory-reset or error feedback. The small power LED
is separate hardware. The S2 blue LED pin remains unverified and disabled.

Compiled-only control patterns require flashing only the S3; the everyday
controls/colours above can be changed and saved at runtime. It does not
change the radio protocol or require updating the installed S2s. Changing
`KMS_TRIGGER_DEFAULT` does not override an existing saved choice; use trigger
recovery to apply the rebuilt default. Runtime preferences and peers share the
S3's `kmswitcher` NVS namespace; factory reset erases only that namespace.

## Physical keyboard lock LEDs

These are separate from the S3 RGB indicator. `KMS_KEYBOARD_LEDS_ENABLED=1`
enables forwarding the selected PC's HID LED state to supported input keyboards.
`KMS_HID_LED_FIELDS`, `KMS_LED_RADIO_MIN_MS`, `KMS_LED_USB_RETRY_MS`,
`KMS_LED_USB_ATTEMPTS`, `KMS_LED_USB_WATCHDOG_MS` and `KMS_LED_USB_REFRESH_MS`
control capacity, coalescing, retries and refresh. See the
[LED settings and hardware test plan](keyboard-led-feedback.md).

The LED feature uses wire format v2/137 bytes, requiring matching S3/S2 firmware.
The S3 and all four S2s were updated on 16 October 2026 with NVS pairings and
the trigger preference retained. K295 Caps Lock indication, control from both
keyboards and opposite-state switching between two PCs pass. Num/Scroll Lock,
wake/reconnect and broader recovery checks remain in the hardware test plan.

## Board changes

An S3 RGB LED may be a WS2812 rather than an ordinary GPIO LED. The documented
YD-style board uses GPIO48. Other boards can use different pins and wiring;
update the board profile and corresponding LED driver if necessary. Flash/PSRAM
pins are not spare output pins.

The S3 uses PSRAM for the input replay ring, radio receive queues, desktop routing
state, retained USB fault snapshots, and the NVS cache. Its live log, diagnostic
history, console snapshots, HID parsers, USB input queues, background management
queues, desktop and clipboard queues, and test capture also use PSRAM.
Task-only allocations prefer PSRAM and fall back to internal RAM when unavailable.
USB DMA storage, interrupt trace hooks, locks and task stacks use internal RAM.
If neither heap can supply optional history or console snapshot storage, those
diagnostics are unavailable while input forwarding continues.

The S2 has no PSRAM and keeps its existing internal-memory policy. Its local
receiver history is excluded from the S3 build; the controller still queries
and displays receiver history normally.

USB VBUS power is a hardware configuration. There is no speculative VBUS GPIO
setting: use the board schematic and actual hub power arrangement.

## Recovery and diagnostic history

Automatic same-computer recovery, the five-second RAM input buffer and hourly
statistics are configurable in `config/kms_settings.h`. See
[connection recovery and retained diagnostics](recovery-and-diagnostics.md)
for the settings, console commands and replay limits. Default master retention is
1,024 events and 72 hourly checkpoints in a dedicated 256 KiB partition. Slaves
retain 256 events and 24 hourly checkpoints in their existing 128 KiB partition.
Receiver journal saves use a 2 KiB buffer to write an inactive flash bank.
The journal header switches banks after all chunks have been written. On
upgrade, receivers import existing single-blob journals. The pre-upgrade copy
is available to older firmware until you explicitly clear the history.
`KMS_SLAVE_HISTORY_BYTES` is the maximum formatted transfer size (96 KiB), not a
RAM allocation; receiver downloads format chunks from a frozen binary snapshot.
The master partition needs a one-time wired update; slaves use
[application OTA](slave-management.md#updating-one-or-all-receivers).
That partition migration starts a fresh diagnostic history and preserves
pairings and preferences.

## Default dongle on boot

An explicitly enabled [test session](stress-testing.md)
temporarily overrides this preference and starts paused across restarts. Send
`test-mode off` after stopping test generators to restore everyday startup.

In browser **Preferences**, load the controller settings, choose **Default dongle
on boot**, then save. Choose Computer 1–7, or Automatic for the first available
paired dongle (the factory default). The choice persists on the S3 and applies
on its next boot without changing the current route. No S2 update is required.

With Insert as your configured trigger, tap **Insert, Insert, D, 2** to make
computer dongle 2 the boot default. Other configured trigger keys work too.
The S3 flashes yellow after D, then flashes blue twice on successful save.
Press 1–7 (top row or keypad) within the selection timeout; Escape cancels.

Console `boot-default` shows the saved choice; `boot-default 2` saves computer 2,
and `boot-default 0` restores Automatic. `status` also shows the saved boot choice.
An unavailable or unpaired default waits until it is paired and USB-ready;
select another available computer manually when needed. Restoring everyday
preferences resets this setting to Automatic and retains pairings. Existing
version 1 preferences migrate with Automatic selected and other settings retained.
