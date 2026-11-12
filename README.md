# KMSwitcher

By [Frank Tzanabetis](https://plasticcheese.com).

Download installers and firmware from [GitHub Releases](https://github.com/RealPlasticCheese/KMSwitcher/releases).
Start with [building](docs/build-and-release.md),
[installation and guided firmware setup](docs/installing.md), and [flashing and pairing](docs/flashing-and-pairing.md).

Original KMSwitcher code and artwork use the [MIT License](LICENSE).
Dependencies retain their own [licenses and notices](THIRD-PARTY-NOTICES.md).

The source builds **Setup & Firmware** (Windows/Linux, no companion required),
the optional **Companion** for edge switching, and a separate Linux **Stress
Toolkit** with its own firmware updater. Both maintenance tools support direct USB and S2 OTA updates.

Keyboard/mouse switcher development for one ESP32-S3 controller and up to seven
paired ESP32-S2 computer receivers, selected with number keys 1-7. Multiple USB
keyboards, mice, and wireless HID receivers feed the same active computer through a USB hub.
See [the agreed requirements and design notes](docs/requirements.md).

The [desktop companion and screen-edge switching](docs/screen-edge-switching.md)
adds station layout configuration, Windows/Linux X11 helpers, tray controls and
a scriptable CLI through any upgraded S2 receiver. Physical deployment and
three-station handoff validation remain separate from software tests.

[Clipboard sharing](docs/clipboard-sharing.md) sends text, formatted text, images,
files and folders to an explicitly selected dongle from the root tray menu. Each
computer chooses dongle-only, network-preferred with fallback, or network-only
content transfers. Plain text is ready to paste on arrival; other content waits
for acceptance in the destination tray.

The implementation includes descriptor-based HID input, a configurable double-tap
shortcut, encrypted ESP-NOW transport, button enrollment, and USB HID output.
See [configuration](docs/configuration.md), [hardware notes](docs/hardware.md),
the [task list](docs/TODO.md), and the retained [diagnostics](diagnostics/README.md).
For setup, follow the [flashing and pairing guide](docs/flashing-and-pairing.md).

## Development tools

- Windows, Git, Python 3.10 or later, and Visual Studio Code.
- ESP-IDF **v6.1**, pinned in `toolchain.json`.
- Espressif Installation Manager (EIM) **v0.20.0**, verified against its SHA256.
- VS Code extensions: `espressif.esp-idf-extension` and `ms-vscode.cpptools`.

The SDK and EIM registry live in `.tools/esp/`; shared compiler tools and
download caches use `C:\Espressif\tools` and `C:\Espressif\dist`.
Python dependencies use the isolated environment created by EIM.
No permanent system PATH or PowerShell execution policy change is needed.

To install the pinned toolchain on another Windows machine, run from this folder:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-toolchain.ps1
```

Internet access is required. Setup may take several minutes and several GB.
On another computer, also install the two editor extensions and set the VS Code
**user** setting `idf.eimIdfJsonPath` to this checkout's absolute
`.tools/esp/tools/eim_idf.json` path. That setting has application scope.
Then use **ESP-IDF: Select Current ESP-IDF Version** to select v6.1, or set the
user setting `idf.currentSetup` to the SDK's absolute `.tools/esp/v6.1/esp-idf`
path. The extension requires that value to match its installation registry
exactly, including Windows path separators.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\idf.ps1 -Project controller
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\idf.ps1 -Project receiver
```

The wrapper activates the pinned SDK for each command. Builds use separate
`build/controller` and `build/receiver` directories, and each project's generated
`sdkconfig`. Supported actions are `build`, `reconfigure`, `menuconfig`, `size`,
`flash`, and `monitor`. Add `-ResetConfig` after changing versioned SDK defaults.
Runtime and board settings are centralised under `config/`.

Open `KMSwitcher.code-workspace` in VS Code. Use **Terminal > Run Build Task**
to choose either chip, or select the relevant firmware folder before using
ESP-IDF extension commands. The default build task builds the controller.

## Connect a board

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\list-devices.ps1
```

Programming port numbers depend on the connected board and operating system. Check before flashing.

For native USB download mode, hold BOOT/FLASH while inserting the board. If it
has RESET/EN, hold BOOT, press and release RESET, then release BOOT instead.
Use the board's documented procedure if its button arrangement differs.
The USB serial interface uses Windows' built-in driver. The S2's separate DFU
interface may show a missing driver; that interface is not used for COM flashing.
See [Espressif's S2 boot-mode guide](https://docs.espressif.com/projects/esptool/en/latest/esp32s2/advanced-topics/boot-mode-selection.html).

Flashing replaces the board's current firmware. When ready, use an explicit port:

```powershell
.\flash-receiver.cmd COM4
.\flash-controller.cmd COM8
```

The receiver wrapper builds, identifies the S2, and flashes using the verified
manual-download/no-reset sequence. Reconnect the S2 normally after flashing.
For wireless updates to installed receivers, see
[Updating one or all receivers](docs/slave-management.md#updating-one-or-all-receivers).
The controller wrapper builds and flashes through its UART bridge. See the
[setup guide](docs/flashing-and-pairing.md) for pairing, MAC checks, and details.
Production S3 logs use its USB-to-UART connector: native USB is the host port.
Production S2 exposes HID only. It does not provide a CDC serial console.
Use the separate diagnostic projects for native USB logs and button/LED tests.

To read the master's retained history and open its console, run:

```powershell
pwsh -NoProfile -File .\scripts\watch-controller.ps1
```

The script discovers the connected CH343 UART bridge even if its COM number
changes, uses the pinned Python environment, and opens without resetting the S3.
Type `help` for all commands, `about` for firmware information, or `.quit` to
close and save the capture under `build/`.
See [console options](docs/recovery-and-diagnostics.md#open-the-console-from-powershell)
for port discovery alone, multiple bridges and unattended captures.

The master console also supports `slaves`, `slave-status N`, `slave-history N`
and `slave-history-clear N`. Use `manage-slaves.cmd` to archive logs or update one
or all paired receivers; see [Updating one or all receivers](docs/slave-management.md#updating-one-or-all-receivers)
for command-line examples. Identical image hashes are skipped unless `--force` is set.
Builds stamp Git/UTC identity and generate `.bin` SHA-256 manifests automatically.
Controller builds bundle matching S2 firmware in a persistent two-slot flash cache.
After the [one-time S3 partition migration](docs/master-updates.md#receiver-firmware-bundled-with-the-master),
use `manage-slaves.cmd update --all --cached` to install it without uploading the
image again from the PC. Browser and Firmware Setup updates refresh the cache too.
OTA-capable receivers require a one-time wired migration; see the
[slave diagnostics and update guide](docs/slave-management.md) for commands,
retention/clearing rules, recovery and the outstanding hardware checklist.

## Engineering Console

The [Engineering Console](docs/web-console.md) provides a live serial terminal
and local firmware build helpers. Use [Companion](docs/installing.md#device-management-in-companion)
for everyday device controls and diagnostics, and [Firmware Setup](docs/installing.md#standalone-firmware-setup)
to flash or update firmware. The console runs offline without a server.
Run `web\package-html.cmd` (requires Node.js), then open
`web/dist/KMSwitcher.html` directly in desktop Chrome/Edge. The generated HTML
contains everything needed and can be copied anywhere without a server.
Click **Connect controller** to manually choose the master's COM/UART port.
For source development, `node web/serve.js` still serves <http://127.0.0.1:8080/>.
The **Build firmware** panel prepares a Windows build launcher or PowerShell
command for the controller, receivers or both. Open the launcher to run the repo's
pinned ESP-IDF build, then choose its application `.bin` and matching `.json` in
the page. The browser cannot start local programs or monitor the external build.
Alternatively, double-click `web/build-all.cmd`, `web/build-controller.cmd` or
`web/build-receivers.cmd`. Builds do not flash hardware. Initial wired installation
and recovery still use the root `flash-controller.cmd` / `flash-receiver.cmd` scripts.
**Explore demo** runs simulated devices without accessing hardware.
The console can be maximised to the browser viewport and restored without
reconnecting. See [master updates](docs/master-updates.md) for the one-time wired
S3 OTA migration, and [preferences](docs/configuration.md#everyday-preferences)
for supported runtime controls. Advanced engineering options remain deferred.

## Board configuration

Esptool identification on 15-16 October 2026 confirmed:

| Role | Chip | Flash | PSRAM | Programming port in this setup |
| --- | --- | --- | --- | --- |
| Controller | ESP32-S3, revision v0.2 | 16 MB | 8 MB on module | COM8 (UART bridge) |
| All four receivers | ESP32-S2FH4, revision v1.0 | 4 MB embedded | No embedded PSRAM | COM4 (ROM downloader) |

The defaults use DIO flash at 40 MHz, each board's detected flash capacity, and
custom production partition tables: S3 factory plus dual OTA slots, and S2 dual
OTA slots. Pairing/data offsets are preserved; see the migration guides. The controller enables
octal PSRAM for a volatile live diagnostic log buffer, recoverable with `logs` in
the console or double-trigger then **L** in a text editor. Press **I** while logs
are printing to enter Keyboard Console; type `help` and Enter for commands, or
press Escape to stop output, or Escape when idle to exit. `history [X [Y]]` also supports minute age ranges in either
console. See the
[recovery guide](docs/recovery-and-diagnostics.md#live-diagnostic-messages-in-psram)
for retention limits and hardware validation status. The compiler target is
`esp32s3`; N16R8 is not a separate compiler target. All four receiver identities
and their paired numbers are recorded in the [setup guide](docs/flashing-and-pairing.md).
Production firmware forwards live input when paired and selected.
The board-check diagnostic drives only the documented S3 RGB pin;
the unverified S2 LED remains disabled in production and board-check. A separate
[S2 LED sweep](diagnostics/README.md#s2-blue-led-gpio-sweep) tests candidate GPIOs
with labelled console output on a spare dongle.

The native USB host port and externally powered hub work with both Logitech
receivers simultaneously. Other hubs and input devices still need verification.
A hub is required for both input receivers together.
The tested hub and two combo receivers use the S3's eight USB host channels in
the current stack. The master now automatically selects up to two complete HID
input devices within that budget, with optional manual
choices in Companion’s **Devices → USB inputs**. Additional devices may not enumerate when all
channels are occupied; see [USB input selection](docs/usb-input-selection.md). See the
[HID compatibility and capacity notes](docs/hardware.md#hid-compatibility-and-simultaneous-input-capacity).

## Operation

- Double-tap your configured trigger, then press **J** to toggle the selected
  computer's mouse jiggler (for example, Insert, Insert, J when Insert is configured).
  Its S2 moves one unit left/right alternately every 30 seconds and keeps running
  after switching away. Set the interval in Preferences or with
  `jiggler-interval SECONDS` in the S3 console. Each S2 restart disables it;
  compatible S3/S2 firmware is required. Real mouse movement or scrolling pauses
  jiggling until the interval has elapsed without activity. Configure the distance
  with `jiggler-distance COUNTS` or in the configuration tool (default 1, range 1–127).
  Double-tap the same trigger, then **K** to cycle that dongle through Left/right,
  Four-way (up/down/right/left), and Random strokes without changing its on/off state.
  Use `jiggler-mode N MODE` in the S3 console, or Companion’s per-dongle
  mode and on/off controls. Random mode runs at about 50 steps/second after the idle
  wait; the other modes use the configured interval between steps.
  See [configuration](docs/configuration.md#mouse-jiggler).
- Tap the same trigger key twice (Ctrl by default), then press 1-7 within three seconds to select a PC.
  Either keyboard controls the shared destination. Keypad digits also work.
- Timeout replays the two trigger taps. Escape cancels silently. Another key replays
  the taps followed by that key. Normal modifier chords retain their meaning.
- Double-tap the trigger, C, then press/release Ctrl, Alt, Shift, Scroll Lock,
  Insert, Pause/Break or Num Lock to save a new trigger. Escape/timeout cancels.
- Double-tap the trigger, P, then 1-7 opens enrollment for that slot for 30 seconds.
  Hold the S2 button for three seconds while its firmware is running.
  Pairing pauses routing; after it succeeds, double-tap the trigger and the number selects
  that receiver. Pairing does not automatically select the newly enrolled S2.
- Three short BOOT presses restore the default trigger with three blue flashes.
  A deliberate startup hold and release factory-resets S3 settings; see the
  [recovery instructions](docs/flashing-and-pairing.md#factory-reset-the-s3).
- Normal S3 RGB indication is off by default; pairing flashes blue. Colours,
  patterns and `KMS_LED_NORMAL_ENABLED` live in `config/kms_settings.h`.
- S3 UART `help` lists all commands; `about` identifies the firmware. Commands
  `status`, `history`, `stats-save`, `trigger-default`, `pair N`, and `select N`
  support setup and diagnostics.
- Double-tap the trigger, D, then 1-7 to save the default boot dongle. The S3
  flashes yellow while waiting, then blue on save. This changes the next boot only.
  Console `boot-default N` or the browser Preferences offers the same setting
  (`0` / Automatic restores first-available selection).
- Slots and generated link keys survive restart. Startup waits for the saved
  default dongle, or selects the first available paired slot in Automatic mode. Any responding paired dongle can be selected, regardless of its computer's USB state.
  Unreachable dongles keep the current destination.
- Switching clears held keys/buttons. Link/USB failure produces five red flashes,
  then recovery continues quietly on the same selected computer. Input buffering
  expires after five seconds; old input is discarded. See
  [recovery and diagnostics](docs/recovery-and-diagnostics.md).
  No USB re-enumeration occurs when switching.

Enrollment uses ephemeral X25519 key exchange and a proof of the derived key;
input uses per-peer ESP-NOW encryption. Enrollment relies on the short physical
pairing window and has no independent identity-verification channel. Pair in a
trusted environment. Pairing cannot yet move an already enrolled receiver to a
different slot; re-pairing/replacing a receiver in the same slot is supported.

Standard media/system keys are represented as HID usages. Proprietary touchpad
gestures and receiver configuration are not forwarded. Physical Caps/Num/Scroll
Lock LED feedback uses descriptor-based output and selected-PC synchronization.
The S3 and all four S2s run protocol v2. The K295 Caps Lock light follows the
selected PC and changes made from either keyboard; the K830 has no physical
Caps Lock indicator. See the [LED feedback and upgrade notes](docs/keyboard-led-feedback.md)
for compatibility and remaining hardware checks.

## Verification

The [Linux hardware test controller](docs/stress-testing.md) manages one or two
synthetic keyboard/mouse S3s, the master UART and any subset of slave slots 1–7.
It provides saved device profiles, a dashboard, simultaneous capture, workload
presets and correlated diagnostic reports. Build the transferable app/firmware
kit with `scripts/build-stress-kit.ps1`; builds do not flash or start a test.
The standard master/slave firmware supports everyday use and software-controlled
[test sessions](docs/stress-testing.md). Everyday startup
honours the saved dongle preference; only a retained test session starts paused.

Both production firmware projects compile with ESP-IDF v6.1. Native tests run
with `python scripts/test-core.py` using the Visual Studio C++ compiler. They
cover shortcut handling, merged input state, malformed/variable HID descriptors,
wire encoding, duplicate rejection, generation changes, receiver leases, and
first-attempt shortcut switching after a dongle disconnects with a busy radio.
See [testing](docs/testing.md) for the local suites and hardware acceptance guide.

## References

- [ESP-IDF v6.1 release](https://github.com/espressif/esp-idf/releases/tag/v6.1)
- [Official Windows installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html)
- [EIM command documentation](https://docs.espressif.com/projects/idf-im-ui/en/latest/cli_commands.html)
