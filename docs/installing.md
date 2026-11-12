# Install KMSwitcher and set up devices

Build from the [public source instructions](build-and-release.md). Packaged releases will be published separately. The following instructions apply to generated packages and firmware bundles.

| Download | Purpose |
| --- | --- |
| **Firmware Setup** (`windows-setup/`, `linux-setup/`) | Initial flashing, regular firmware updates, recovery, and dongle restart |
| **Companion** (`windows-companion/`, `linux-companion/`) | Device management and diagnostics, with optional screen-edge switching and clipboard features |
| **Engineering Console** (`console/KMSwitcher.html`) | Live controller terminal and local firmware build helpers |
| **Stress Toolkit** (`linux-stress/`) | A separate Linux rig, generator flashing, test-firmware updates, test dashboards and reports |

Firmware setup never requires the Companion. Keyboard switching works without
desktop software. Setup and the Stress Toolkit do not enable login startup.

## Standalone Firmware Setup

On Windows, run the installer from **windows-setup**, or extract its portable ZIP
and open **kms-setup.exe**. On Linux, open the DEB/RPM in **linux-setup** with your
package manager. The tar archive includes `install.sh`. Open **KMSwitcher Firmware
Setup** from the application menu. Reconnect the programming cable after
installing Linux USB access rules.

Choose **Set up a new device**, **Update firmware**, or **Recover a device**:

1. Import the regular firmware ZIP, or choose an extracted bundle containing `bundle.json`. No firmware is embedded in the application.
2. Select S3 controller or S2 receiver and choose the connection method.
3. **Direct USB:** connect the board locally. Hold the S2 button while plugging
   it in, release, refresh ports, and select its programming port. No controller
   is required. S3 programming uses its COM/UART connector.
4. **Application update through controller:** connect the S3 COM/UART cable.
   Leave S2 receivers powered and paired in their usual computers. Inspect,
   tick the devices to update, then review/update. The checkboxes choose the
   targets for this method, regardless of the device-type dropdown. When both
   controller and dongles are checked, the dongles update first.
5. Review release, targets, method and port. The tool verifies firmware and
   reports individual results. Reconnect normally after direct USB.

Controller flashing is optional: its checkbox starts unchecked, like the dongles.
Tick it explicitly before a controller update or direct USB flash. For dongle-only
updates, choose the application/OTA method through the controller and tick only
the dongles you want to update; the controller application is left untouched.

To use the S2 image already stored on the S3, choose the controller UART update
method, then **Firmware source → Receiver firmware stored on S3**.
Inspect the controller to see the stored build, size and SHA-256, tick the
dongles, and review/update. No local firmware files are required or uploaded.
The controller checkbox is disabled for this source. If the stored image changes
after inspection, the update stops and asks you to inspect again. Empty caches
and controllers without cache support are reported without uploading a fallback.
Changing the port clears the inspected image and device selections.

The installed CLI supports the same operation:

```powershell
kms-firmware ota --port COM8 --role receiver --slot 3 --cached
```

Use direct USB for first installation, older layouts, partition migrations and
recovery. Supported updates preserve pairing/settings; incompatible layouts are
rejected. Input through updated devices is interrupted. Wired updates keep private
pairing/settings backups under the KMSwitcher configuration directory.

To store an S2 image without updating any dongles, select the controller UART
method and use **Store receiver firmware on S3**. **Choose local application**
accepts an OTA application `.bin` with its matching `.json` manifest beside it.
The image must match the selected target role. **Save results JSON** exports
per-device results after an update or restart.

**Pair & configure** provides instructions for pairing the first dongle. It also
opens the separately downloaded [Engineering Console](web-console.md) for raw
UART commands. Finish maintenance operations before opening the console, and
disconnect the browser’s serial session before using the same port in Setup.

## Getting newer firmware

Both maintenance products support **Import firmware ZIP** and **Get updates**.
Import a firmware bundle built from the matching source revision.
Regular and stress bundles are checked separately.
Compatible firmware-only updates do not require an application reinstall.

For optional public hosting, copy the firmware artifact contents, including `release-index.json` and its ZIP,
together to HTTPS hosting, then enter that index address once in **Get updates**.
**Check for updates** lists releases; **Download selected firmware** verifies and
imports the bundle. Stable releases are the default. Development builds require
an explicit choice. Publish public files only from a successful complete release.

## Separate Stress Toolkit

Install the DEB/RPM from **linux-stress/**, or use its tar installer. Open
**KMSwitcher Stress Toolkit** from the application menu. The tar payload's
`kms-setup` launcher selects stress mode automatically.

Start with **Prepare test rig**, then **Firmware** to program the controller,
receivers and each generator. Receivers support OTA and direct USB. Generators
use direct USB; choose only the spare S3 intended as an input generator. The
bundle includes bootloaders and partition tables for first-time flashing.

Open the test dashboard, discover devices, review identities/slots and save the
profile. Run direct-generator Baseline first, move generator USB to the controller
hub, then run Quick or longer tests. The toolkit provides desktop/browser
dashboards, saved reports and report ZIP exports. Use **Stop browser dashboard**
before firmware maintenance. A hardware lock prevents overlapping test/update
sessions. Profiles/reports survive upgrades/removal. Live capture requires Linux.

## Optional Companion

Download the Companion packages from its successful component pipeline or the
**Assemble Downloads** run. Extract the download before running an installer. Python,
ESP-IDF and a source checkout are not needed.

For companion-reported PC state and automatic jiggler suspension, see
[computer power reporting](computer-power.md).

## Windows

For edge switching and clipboard features, open
`windows-companion/KMSwitcher-Companion-VERSION-windows-x64-setup.exe`. The installer installs
for your account and adds Start menu shortcuts. Starting at login and adding
the CLI to your user PATH are optional. At the end, leave **Launch KMSwitcher
Companion** selected to open the desk layout and tray app. You can open firmware
setup from the Start menu or the companion’s **Open Firmware Setup** button.
If Setup is installed in a custom location or run from a portable archive,
choose **Locate Firmware Setup** to remember its launcher.

The portable ZIP contains the same application. Extract it and open
`kmswitcher-gui.exe`. The console command is `kmswitcher.exe`.

## Linux

On Mint, Ubuntu or Debian, open the `.deb` in the software installer. On Fedora,
open the `.rpm`. The package manager installs the required desktop libraries
and USB access rules. Reconnect the receiver/programming cable after installing,
then open **KMSwitcher** from the application menu. Some desktop package tools
only install local packages through the terminal:

```sh
sudo apt install ./kmswitcher_VERSION_amd64.deb
# Fedora:
sudo dnf install ./kmswitcher-VERSION.x86_64.rpm
```

The tar archive includes `install.sh` and `START-HERE.md`. Extract it, then run
`sh install.sh`; it requests administrator access to install the desktop entry,
command and USB rules. On other distributions install the equivalent libraries
listed in the native package dependencies first. The portable uninstaller is
`sudo /opt/kmswitcher/uninstall-portable.sh`; use your package manager to remove
a DEB/RPM installation. Local preferences are retained.

Linux binaries target x86-64, glibc 2.35 or newer. Automatic edge switching
requires X11. On Wayland, firmware setup and CLI configuration remain available,
but automatic switching needs a future desktop backend. A tray icon appears
when the desktop supports it; otherwise the settings window remains available.

## Guided firmware installation

The wizard includes both firmware images from the same source commit as the app:

1. Select **S3 controller** or **S2 receiver**.
2. For an S3, connect its **COM/UART** socket with a data cable. Keep the hub on
   its native USB socket. If needed, hold BOOT, press/release RESET, release BOOT.
3. For an S2, unplug it, hold its button while reconnecting, then release. It
   appears as a programming port; its normal HID mode has no serial port.
4. Choose **Direct USB**, refresh the port list, select the connected board, then
   **Review and update selected device(s)**. The tool checks chip, capacity, firmware checksums
   and partition layout before writing. It backs up pairing/settings NVS,
   verifies the written firmware and confirms that NVS is unchanged.
5. Reconnect the board normally without holding BOOT. Repeat for the other boards.

Normal updates preserve pairing and preferences. There is no full erase option.
Unknown/incompatible existing layouts are rejected before writing; do not erase
a working board to bypass that check. Backups are stored in the local
KMSwitcher configuration directory under `firmware-backups/`; keep these private
because they contain pairing keys. An interrupted flash can be retried in BOOT
mode. Use a spare keyboard/mouse while updating the active input path.

If a port is missing, use a data cable, reconnect in programming mode, and close
other serial tools. On Linux reconnect after installing permissions. Windows
normally obtains UART bridge drivers through Windows Update; if Device Manager
shows an unknown bridge, install its manufacturer's driver for that board.

## Pair and arrange the desk

The last wizard page explains pairing: double-tap Ctrl (or your saved trigger),
press P, then a receiver number 1–7. Within 30 seconds hold the running S2's
button for about three seconds and release. Double-tap the trigger and press
the number to select it. Existing pairings survive normal firmware upgrades.

Run the companion on each computer. Arrange the dongle stations in Settings and
save to the master. Station identity follows the dock's dongle. Automatic
switching only runs when placement and enabled, ready companion sessions exist.
Manual hotkeys remain available. Companion Settings can enable login startup; the tray
can pause automatic switching locally.

## Scripting

```sh
kmswitcher --version
kmswitcher --json build-info
kms-firmware --bundle firmware.zip check
kms-firmware ports
kms-firmware --bundle firmware.zip flash --role receiver --port COM4
# Linux: --port /dev/ttyACM0
kmswitcher --json status
kmswitcher auto disable
kmswitcher startup enable
```

`kms-firmware flash` supports `--expected-mac` for scripted board identification and
`--bundle PATH` for an extracted release firmware folder. Progress goes to
stderr; the final result is JSON on stdout. Errors return a nonzero exit code.

## Independent firmware and application versions

Companion, Firmware Setup, Web Console and firmware versions advance independently.
A new firmware ZIP does not require reinstalling applications. The ZIP always
contains a matched S3 controller/S2 receiver pair, including the receiver cache
written to the controller. Never assemble a bundle by mixing builds.

Firmware Setup accepts ZIPs on the CLI: `kms-firmware --bundle release.zip check`.
Use Firmware Setup for application updates, including updates through the controller.

Application updates offer Conservative, Balanced and Fast dongle profiles. See
[Shared stream transport](stream-transport.md) for radio settings, compatibility,
buffer ownership and the distinction between radio rates and file throughput.

## Device management in Companion

Open **Settings → Devices** with a paired dongle attached. **Preferences** edits
controller settings, including the named per-dongle colours, startup dongle and
shortcut timing. **Restore preference defaults** keeps pairings, USB input
choices and history. **Dongles** opens pairing, removes saved pairings, pauses or
resumes routing, and controls each dongle’s mouse jiggler. **USB inputs** shows
the controller’s input devices, channel use and automatic/manual selection.

Use **Radio** to change the controller’s channel, and **Diagnostics** to view
local dongle events or inspect, download and clear retained history on a paired
remote dongle. These controls use the attached dongle’s USB connection and do not
need the controller’s UART cable. The controller and attached dongle need
compatible firmware for the new controls. Use Firmware Setup to update them.
If there is no paired dongle yet, follow Firmware Setup’s pairing instructions.

Automatic switching and starting at login are optional. From **General**, open
the same installed Firmware Setup and the same downloaded Engineering Console
used independently of Companion.
