# USB-only stall reproducer

This diagnostic image uses USB Host 1.6.0 with driver tracing and an optional frame guard.
Read the [driver trace instructions](usb-driver-trace.md) for instrumentation details.
Use a spare ESP32-S3 as the test host and separate synthetic HID generators.
Select boards by their actual identities before flashing.
The isolation identity query has four bounded attempts and services cancellation while waiting.

## What the minimal image contains

The image uses ESP-IDF 6.1 and exactly `espressif/usb` 1.6.0 with a bounded HCD
observer and a runtime-selectable experimental HAL channel-start guard. It opens only VID:PID `303a:4006` generators, claims their interrupt IN
endpoints directly, and verifies the tagged 54-byte payload, sequence and CRC.
The generator pattern and verifier are also compared with an independent Python
oracle in the native regression suite.

There is no KMS router, HID parser, ESP-NOW/Wi-Fi startup, S2 transport, NVS write,
PSRAM setup, production USB wrapper, or automatic USB recovery. The standard SDK
hub implementation and enumeration control requests remain present. UART output
runs separately from the USB client; SDK buffers stay owned until cancellation
callbacks drain. A failure freezes a first-fault snapshot and six preceding bus
samples. Registers are read without acknowledging interrupts or popping RX data.
The SDK stays running so a late completion can still arrive. The image does not
restart or reconnect the USB bus to hide a failure.

Runtime modes all use the same image:

| Mode | Additional traffic after enumeration |
| --- | --- |
| `input` | Generator interrupt IN reports only; no application controls |
| `probe` | GET_STATUS once per second per generator |
| `led` | HID LED SET_REPORT once per second per generator |
| `both` | Alternating probe/LED requests, each type once per second |

## Build and package on Windows

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project usb-isolation
python scripts/package-stress-kit.py --usb-isolation
```

This builds without flashing. The resulting `build/KMSwitcher-usb-isolation.zip`
contains app 0.3.9, the minimal master image, bootloader, partition table, hashes,
instructions and a standalone source copy in `reproducer/`. The source copy can
be built with an activated ESP-IDF 6.1 installation from
`reproducer/diagnostics/usb-isolation` using `idf.py set-target esp32s3` and
`idf.py build`. It does not require the full KMSwitcher repository or Git metadata.

## Back up and flash the spare master

Close serial consoles and the test app. Connect the spare master's USB-to-UART
connector to Windows. Identify its COM port by comparing these results before
and after connecting it:

```powershell
$espPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $espPython -m serial.tools.list_ports -v
$sparePort = 'COM12' # Replace with the spare master's actual port.
& $espPython -m esptool --chip esp32s3 --port $sparePort read-mac
```

Identify the spare master and both generators by their actual MACs before flashing.

Extract the package and change to its top-level directory. Back up all 16 MiB
before changing the spare master's partition table. Keep that private backup
outside the package/report directories; it includes saved settings and keys.

```powershell
& $espPython -m esptool --chip esp32s3 --port $sparePort --baud 460800 `
  read-flash 0x0 0x1000000 C:\temp\spare-master-before-isolation.bin

& $espPython -m esptool --chip esp32s3 --port $sparePort --baud 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-freq 40m --flash-size 16MB `
  0x0 firmware/usb-isolation-bootloader.bin `
  0x8000 firmware/usb-isolation-partitions.bin `
  0x10000 firmware/kmswitcher_usb_isolation.bin
```

The image uses a single factory partition so existing OTA selection cannot boot
the normal controller accidentally. It leaves the NVS/PHY regions unwritten.
Restore the exact previous firmware, partition table, OTA state and preferences
after the experiment by writing the backup back to the same board:

```powershell
& $espPython -m esptool --chip esp32s3 --port $sparePort --baud 460800 `
  --before default-reset --after hard-reset write-flash `
  0x0 C:\temp\spare-master-before-isolation.bin
```

## Run on Linux

Extract the app files over the existing kit directory containing `linux/`.
The saved profile supplies master/generator chip identities and UART paths;
receiver entries are ignored. Close the dashboard so its UARTs are free.
The runner refuses a normal controller image, a mismatched chip, or an unexpected
native-USB generator inventory before starting traffic.

Keep the same hub, generator ports, cables, power and workload seed as the normal
campaign. Connect the generators' native USB ports to that hub and all three
UART connectors to Linux. The generators are never connected directly as input
devices to Linux during this test. S2 connections may stay in place but are unused.

From `linux/`, begin with input only:

```sh
bash run-usb-isolation.sh --mode input --seconds 3600 --rate 30
```

The default profile is `~/.config/kmswitcher-stress/profile.json`; use
`--profile /path/to/profile.json` for another. `--output` selects the report
directory and `--pattern media` reproduces the campaign's media payloads.
Ctrl+C requests cleanup; generators also retain their two-second heartbeat lease.
No receiver input grabs, pairing commands, selection commands or board resets
are sent by the runner.

After a healthy run, repeat with `--mode probe`, then `--mode led`, then
`--mode both`, keeping the same duration and rate. Compare equal exposure and
repeat promising differences: a quiet hour is not proof that the cause is fixed.
After a failure, retain the report before power-cycling the spare master to start
another run. It intentionally has no automatic bus recovery. A blocked neutral
release is saved as a cleanup error instead of silently passing.

## Evidence and interpretation

The runner saves results and diagnostic evidence locally.
Keep lab captures outside the public repository and review exported reports before sharing.
A passing workload establishes behavior for that workload and exposure.
