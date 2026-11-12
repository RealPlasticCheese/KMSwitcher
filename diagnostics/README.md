# Retained diagnostics

These programs remain separate from the switcher firmware.

| Project | Purpose |
| --- | --- |
| `bringup` | Original chip-information and heap heartbeat program |
| `board-check` | Button edge/long-press logs; S3 RGB colour cycle; X25519, SHA-256 and HMAC self-test |
| `led-sweep` | S2-only GPIO LOW/HIGH sweep with TinyUSB console labels to investigate the blue LED |
| `../tests/native` | Host tests for HID parsing, shortcut routing, wire encoding and duplicate/lease handling |

Build the bring-up and board-check diagnostics for either chip:

```powershell
.\scripts\idf.ps1 -Project board-check -Target esp32s2
.\scripts\idf.ps1 -Project board-check -Target esp32s3
.\scripts\idf.ps1 -Project bringup -Target esp32s2
```

Add `-Action flash -Port COMn` to load it and `-Action monitor -Port COMn` to
read it. Check ports with `scripts/list-devices.ps1` each time. Diagnostic builds
have separate output directories and `sdkconfig` files per chip, so they do not
change production USB/console settings. A native USB flash may need a manual
reset or reconnect to run. Exit the IDF monitor with Ctrl+].

S2 board-check uses TinyUSB CDC (observed as COM5); its ROM download port was
COM4. S3 board-check uses native USB Serial/JTAG (COM6). Always enumerate ports
again. The original `bringup` project retains its original console choices;
its S2 ROM CDC console failed enumeration on the connected board, so use
`board-check` for current S2 tests.

On S3, expect red, green, yellow, blue, magenta, cyan, and white in that order.
The brightness and dwell time are configured under `config/`. A BOOT press and
release should produce GPIO0 log messages; holding it for three seconds should
produce one long-press message. On S2, LED driving remains disabled until the
KEY V1.0 wiring is established. Its button can still be tested through logs.

`scripts/serial-capture.py --port COMn --seconds 10 --output build/capture.log`
provides a bounded capture and leaves BOOT/RESET control lines deasserted.
Run it with the pinned ESP-IDF Python, which has pyserial installed. Add `--dtr`
for the running S2 TinyUSB console if needed; BOOT/RESET remains deasserted by
default. Its optional
`--command 'status'` or `--command 'pair 1'` targets the production S3 UART console.
For continuous bench work, add `--interactive --seconds 1200`: each stdin line
sends a console command while capture continues, and `.quit` closes the port and
saves the log. This avoids losing button or reboot messages between captures.
For unattended controller captures, `--status-interval 5` polls radio status every
five seconds without needing terminal input. The interval is in seconds and can
be changed on the command line; status polling does not change the selected slot.
For an intentional short-reset recovery test on the S3's external UART bridge,
add `--uart-reset --reset-hold-ms 100`. This pulses EN using the pinned esptool's
reset helper while leaving BOOT deasserted. Never use it on a native USB port.
Normal captures do not reset either board. The pulse duration is configurable.
Controller `status` also reports enumerated USB device/interface counts and each
supported HID interface's received/decoded report totals. These counters show
whether input reaches the parser without recording key values or pointer data.

## S2 blue LED GPIO sweep

`led-sweep` is a separate ESP32-S2-only investigation image for an otherwise
unattached KEY V1.0 dongle. Production firmware and `board-check` keep
`KMS_S2_LED_GPIO` disabled; an always-on LED under those images does not establish
whether it is wired to a GPIO or directly to a supply rail.

The candidate list and timing settings are in `config/led_sweep.h`. The default
list tests GPIO1-18, GPIO21, and GPIO33-44. It excludes BOOT/strapping pins
GPIO0/45/46, USB GPIO19/20, flash/PSRAM GPIO26-32, and nonexistent GPIO22-25.
GPIO46 is also input-only. See
[Espressif's S2 GPIO reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s2/api-reference/peripherals/gpio.html).
The dedicated SDK defaults disable the UART console so GPIO43/44 are available;
logs use the same TinyUSB CDC implementation as `board-check`. The code rejects
pins outside these ranges even if the candidate list is edited.

These are chip-level exclusions, not verified KEY V1.0 board wiring. Remove any
candidate connected to other hardware before testing a different board. The
diagnostic uses minimum GPIO drive strength, which is not a current limiter.

Build from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\idf.ps1 -Project led-sweep -Target esp32s2
```

Hold the dongle's BOOT button while reconnecting it, then release it. Enumerate
ports with `scripts/list-devices.ps1` and replace `COMn` with its ROM download
port. Flashing replaces the application on that dongle; use the spare, not a
working receiver. This project's defaults select no reset before/after flashing,
matching the KEY dongles' manual download procedure.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\idf.ps1 -Project led-sweep -Target esp32s2 -Action flash -Port COMn
```

Reconnect normally without holding BOOT. Enumerate ports again: the running
TinyUSB CDC console can have a different COM number. The console helper uses the
pinned Python environment, opens at 115200 with DTR asserted, and saves streaming
output to `build/led-sweep-console.log`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\watch-led-sweep.ps1 -Port COMn
```

The sweep waits for a connected console with DTR asserted, then gives three
seconds to start watching. It logs the GPIO and electrical level at each step:

```text
Testing GPIO1; 3 LOW/HIGH cycles
GPIO1 LOW (1/3); hold 1000 ms
GPIO1 HIGH (1/3); hold 1000 ms
...
GPIO1 released: input, no pulls
Testing GPIO2; 3 LOW/HIGH cycles
```

The helper captures for 240 seconds by default; use `-Seconds N` for a different
duration or Ctrl+C to stop early. It does not reset or flash the dongle.

Each candidate is LOW for one second, then HIGH for one second, repeated three
times. Only one candidate is driven at a time. It is released to an input with
no pulls before a half-second gap and the next candidate. A default pass takes
about 3 minutes 22 seconds, then repeats after three seconds. Closing the console
or dropping DTR releases the current pin within a polling interval (20 ms plus
scheduling); reopening starts again at the first candidate. Unplugging stops the
test. No radio, pairing, HID output, or NVS writes are used by this image.

Watch for a repeatable LED change and record the GPIO and level. LOW may mean
on or off, depending on wiring. No visible response only rules out this simple
drive test on the tested candidates; it does not prove the LED is power-only
or rule out a different LED driver. For a conclusive wiring check, disconnect
power and trace the LED's series resistor with a continuity meter. Confirm any
candidate before changing the production board profile. Restore receiver
firmware with `flash-receiver.cmd` when finished.

The KEY V1.0 LED wiring needs a physical continuity check before selecting a production GPIO.

## Windows shortcut output check

`windows/check-shortcut-input.py` uses Windows Raw Input to observe the PC-facing
keyboard. It records only Ctrl and digit transitions from device paths matching
`--device-match`; it does not log other key values, inject input, suppress keys,
or change focus. It creates a hidden message window and exits after `--seconds`.

```powershell
python .\diagnostics\windows\check-shortcut-input.py --device-match 'vid_303a&pid_4005' --seconds 60 --output build\shortcut-output.log
```

Use the receiver VID/PID currently configured in `config/kms_settings.h`, or a
more specific device-path substring when several matching receivers are attached.
The default `--poll-ms 5` requests a five-millisecond message-pump interval.
Timestamps include polling and scheduler uncertainty; use this for functional
shortcut/release checks, not precise end-to-end latency measurements.
First use an ordinary Ctrl chord to verify the observer receives events. Double
Ctrl plus a selection digit should suppress those transitions; a timeout should
produce two Ctrl down/up pairs together after the selection window expires.
An out-of-mode digit checks normal forwarding. The implementation follows
[Microsoft's Raw Input guidance](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input).

## Flash backups

Original board flash backups, when captured, are kept in the ignored
`.tools/board-backups/` directory. They can contain factory device-specific data
and are not committed. Diagnostic source and dependency lockfiles are versioned.

`scripts/backup-flash.py --chip esp32s2 --port COMn --size 0x400000 --output PATH`
is a read-only investigation helper for the native USB read stall observed on this S2.
It uses the pinned esptool APIs, small chunks, and one acknowledged sector in
flight. Enter ROM download mode manually first. It refuses to overwrite an
existing backup, verifies each chunk and the whole-image flash digest, and writes
SHA-256/MAC metadata alongside a completed image. `--length 0x80000` reads only
the first 512 KiB, for example to preserve the region overwritten by board-check;
this is a region backup, not a complete flash image. No installed SDK files are
modified. Sizes, chunk length, and transfer window are command-line options.
This reduced-window approach also stalled on the connected S2 and is not a
verified workaround. A backup file is saved only after a complete verified read.
