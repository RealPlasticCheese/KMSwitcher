# Flashing and pairing

Run commands from the repository folder on the Windows development PC. The
pinned toolchain must already be installed with `scripts/setup-toolchain.ps1`.
The wrappers activate or locate that toolchain automatically.

Keep both firmware roles on matching protocol versions.
See the [coordinated S3/S2 update notes](keyboard-led-feedback.md#firmware-compatibility-and-updates).
Label each receiver with its paired number. Slots 1 through 7 are assigned during pairing.
Use the actual identity reported by each board when selecting or flashing hardware.

## Find the programming port

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\list-devices.ps1
```

Port numbers can change. COM4 for the S2 downloader and COM8 for the S3 UART
bridge are examples from the tested setup, not required port assignments.
Close serial monitors/captures before flashing a device on the same port.

## Flash an S2 receiver

For wireless updates to already installed OTA-capable receivers, see
[Updating one or all receivers](slave-management.md#updating-one-or-all-receivers) for the
`manage-slaves.cmd` commands. Use the wired procedure below for initial
installation and recovery.

1. Unplug the S2. Hold its button while plugging it into the development PC,
   then release the button. This starts the ROM downloader.
2. Find its COM port and run:

   ```powershell
   .\flash-receiver.cmd COM4
   ```

3. Wait for the verified-flash success message. Unplug and reconnect normally,
   without holding the button, to run the receiver firmware.

The command builds the receiver, identifies the chip/MAC, and uses the manual
download/no-reset sequence verified on the KEY dongles. Image offsets, capacity,
mode, and frequency come from ESP-IDF's receiver build files. Failed commands
return a nonzero exit status. The running receiver appears as keyboard, mouse,
and media/system HID interfaces, with no COM port. That is expected.

Optional checks and reuse of an existing successful build:

```powershell
.\flash-receiver.cmd COM4 -ExpectedMac 02:00:00:00:20:03
.\flash-receiver.cmd COM4 -SkipBuild
```

`-ExpectedMac` refuses a different board before writing. `-SkipBuild` uses the
existing receiver build, so omit it after source/configuration changes. The
PowerShell implementation is [scripts/flash-receiver.ps1](../scripts/flash-receiver.ps1).
Ordinary updates preserve pairing in NVS. The helper does not run a full erase.
Every receiver uses the same image; its computer number is assigned by pairing.

When out of USB ports, temporarily borrow the working S2's PC port. Input through
that receiver pauses while it is unplugged. After programming, move the new
receiver to its PC, return the original, and select its number again. Receivers
can also be paired/tested on the development PC before being moved to future PCs.

## Flash the S3 controller

Connect its **COM/UART** connector to the development PC. Keep its native USB
connector available for the powered hub and input receivers.

```powershell
.\flash-controller.cmd COM8
```

This builds and flashes through the UART bridge using `scripts/idf.ps1` and the
pinned SDK. If automatic download mode fails, hold BOOT, press/release RST, then
release BOOT and retry. After programming, normal startup needs BOOT released.

## Pair a receiver to a number

For a separate test rig, keep its master on channel 6 while pairing and
[OTA-updating older spare receivers](slave-management.md#updating-one-or-all-receivers). Upgrade all paired receivers before
changing the rig's [saved radio channel](radio-channels.md).

Choose a number from **1 through 7**. Power the S3 with its input keyboard/hub
connected, and plug the running S2 normally into a computer's USB port.
The instructions below assume the default Ctrl trigger; substitute your current
trigger if you have changed it. The S3 RGB LED flashes **blue** during pairing.

1. On the keyboard, quickly tap and release the same Ctrl key twice.
2. Press **P**, then the chosen number. The S3 opens a 30-second pairing window.
3. While that window is open, hold the S2 button for about **three seconds**, then
   release it. Both pairing windows must overlap. Do this while the S2 firmware
   is running; holding its button during insertion starts the downloader instead.
4. After pairing completes, double-tap Ctrl and press the number to select it.
   Test typing and mouse movement on its computer.

Successful pairing does not automatically select the newly paired receiver.
Pairing pauses input routing until a destination is selected again. If a window
expires, repeat the sequence and then select the desired receiver. To resume
another already paired PC, double-tap Ctrl and choose its number.

The S2's blue LED is not currently used for pairing feedback because its GPIO is
unverified. For explicit confirmation, use the S3 UART console: successful pairing
logs `Paired computer N`, and `status` shows its MAC and USB readiness. Console
commands `pair N` and `select N` provide the same setup/selection actions.
The retained `scripts/serial-capture.py` can send these commands; see
[diagnostics](../diagnostics/README.md).

Read `last seen` as well as `USB`: readiness is the receiver's last reported USB
state, so a stored `USB=ready` with an old timestamp does not establish that an
unplugged receiver is online.

Pairing stores the slot and link keys on both devices and survives ordinary
power cycles/firmware updates. It belongs to the S2, so moving that receiver to
another PC keeps its number. Replacing/re-pairing a receiver in the same slot is
supported; moving an already paired receiver to a different slot is not yet
implemented. Pair in a trusted environment during the physical setup window.

## Normal switching

Double-tap the current trigger (Ctrl by default), then press **1-7** within three
seconds. Either keyboard controls the shared destination. With no selection,
timeout replays the two trigger taps.
Escape cancels; an unavailable slot keeps the current destination. Switching
clears held input, and a held selection key/button stays suppressed until
released. Both input receivers remain on the S3 hub throughout normal use.

Timings, capacities, radio settings, and board wiring defaults are documented
in [configuration.md](configuration.md). Hardware evidence and remaining checks
are in [testing.md](testing.md) and [TODO.md](TODO.md).

## Change the double-tap key

1. Quickly tap and release the current trigger twice, then press and release **C**.
2. On the keyboard that pressed C, press and release **one** of: Ctrl, Alt,
   Shift, Scroll Lock, Insert, Pause/Break or Num Lock. Do this within ten seconds.
3. Two blue flashes confirm the new choice was saved. It applies to both
   keyboards and survives power cycles.

For example: **double-Ctrl, C, Scroll Lock** changes the trigger to Scroll Lock.
**Double-Scroll Lock, C, Escape** leaves Scroll Lock selected. Escape or timeout
cancels silently at the PC, with one yellow confirmation flash. Configuration
consumes its command keys and does not replay the old trigger taps. An unsupported
key or a chord is ignored; release it and choose an allowed key before timeout.
Input from another keyboard cannot set the new trigger. Save failure flashes red
and keeps the old choice.

For modifier triggers, either side works, but both taps must use the **same
physical key on the same keyboard**. Ordinary chords keep their meaning. A
standalone trigger tap may be delayed by the 350 ms detection window; the full
double tap is retained for the three-second selection window. These keys can
still have application shortcuts, and lock keys can affect lock state when
replayed. Supported keyboard lock indicators follow the selected PC's reported
state; see the [LED compatibility notes](keyboard-led-feedback.md).

The RGB LED is **off during normal forwarding**. Selection/configuration flashes
yellow; pairing flashes blue. An unavailable-computer selection or link failure
produces five red flashes, each 125 ms on and 125 ms off. The sequence runs once
per outage; recovery then continues quietly on the selected computer. Later fault
reasons and expired buffered input do not restart the warning. Enable normal
slot-colour indication by changing
`KMS_LED_NORMAL_ENABLED` to `1` in `config/kms_settings.h` and reflashing the S3.

## Recover a forgotten trigger

With the S3 running, tap **BOOT three times within two seconds**, releasing it
between presses. Each press must be shorter than half a second. Three blue
flashes confirm restoration to the compiled default (Ctrl). Paired receivers
and the selected computer are retained. The UART command `trigger-default`
performs the same recovery, and `status` prints the current trigger.

## Factory reset the S3

This clears all S3 pairings and its saved trigger. Re-pair the S2s afterward.

1. Power on or reset the S3 **without holding BOOT**. Wait for the brief white
   startup indication, then begin holding BOOT within ten seconds of readiness.
2. Keep BOOT held for five seconds. The RGB LED blinks red during the hold,
   then turns solid red when ready.
3. Release BOOT to confirm. The S3 clears its KMSwitcher settings and restarts
   with the default Ctrl trigger and no paired computers. Releasing before the
   solid red indication cancels the reset.

Holding BOOT while power is applied/reset is pressed starts the chip's ROM
downloader, so the reset gesture deliberately begins **after normal startup**.
RST alone remains an ordinary restart. Factory reset does not erase firmware
or unrelated NVS namespaces and cannot erase the records on disconnected S2s;
put each running S2 into pairing mode again to enroll it. Individual forgetting
and moving an enrolled S2 to another slot remain future work.

## Detected by Windows but input fails through a USB-C adapter

Check the S3's `status` output as well as Windows Device Manager. A receiver may
enumerate correctly as HID while its radio link is unreliable. `last seen`
should stay fresh; repeated multi-second gaps need investigation even if the
last reported USB state says ready.

Try a USB extension cable on the same adapter and place the S2 away from the
adapter, display cable and charger. After reconnecting, double-tap Ctrl and select
its number again. In the tested generic three-in-one adapter, receiver 3 failed
when attached directly but worked through an extension, with its display and
charger connections retained. Radio status also became steady. No new firmware
or re-pairing was required for that comparison.

Local interference or antenna placement is the leading explanation in that
setup. Some USB 3 equipment emits noise affecting 2.4 GHz radios; see the
[Intel interference paper](https://www.usb.org/sites/default/files/327216.pdf).
The test does not establish the exact noise source. A hub can also help, but
changes USB topology and power conditions as well as receiver placement.
