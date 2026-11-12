# USB Host 1.6.0 driver trace experiment

This spare-master image instruments espressif/usb 1.6.0 on ESP-IDF v6.1.
The registry still lists 1.6.0 as the latest published version on 23 October 2026:
https://components.espressif.com/components/espressif/usb/versions/1.6.0/readme
It includes diagnostic instrumentation and an experimental, runtime-selectable
frame-boundary workaround. The workaround is a candidate, not a proven stall fix.
The combined trace kit now
updates the spare master and both synthetic generators. The regular S3 controller
and S2 receiver firmware are unchanged. The 1.5.0 comparison
remains separate. Do not mix a driver-version change with a hub/topology change
when interpreting comparative failure rates.

## Evidence collected

The trace stores 128 recent events and six periodic samples in static internal
RAM. It counts events per physical channel independently of ring overwrites.
Normal traffic produces no trace UART output. An observed first fault captures
one final sample and freezes the store; late completions cannot rewrite it.
A successful new `iso arm` resets storage. The ordinary host remains running;
there is no added halt, flush, reconnect, register acknowledgement or DMA write.

Events (kind):

| Kind | Observation | Meaning |
| --- | --- | --- |
| 0 | HCD enqueue accepted | URB added to pending queue after validation |
| 1 | HAL channel activation returned | DMA activation invoked for buffer/stage |
| 2 | Channel ISR before HAL decode | Raw HCINT bits before normal acknowledgement |
| 3 | HCD buffer parsed | URB about to enter done queue; transfer status recorded |
| 4 | HCD dequeue | URB removed from done queue for upper-layer handling |

`object` is the usb_transfer_t address, allowing correlation with each DEVICE
`transfer` field. DEVICE submissions/callbacks separately count application
submit success and callback entry since arm. An existing pre-arm pending read
can produce a callback without a new run submission; compare deltas rather than
assuming these counters always match exactly.

Each sample retains root interrupt count/last timestamp, raw USB/frame/channel
registers, pipe state, HAL flags, multi-buffer flags, pending/done counts, current
read-buffer/URB identity, current QTD index/status/data address, and periodic frame
list scheduling bits. Pipe `endpoint` packs device address in bits 8+ and endpoint
address in the low byte. `schedule-mask` has one bit per 32-frame list entry that
enables that channel. Only allocated pipes tracked under the HCD lock are read;
freed channels are removed before deallocation. No register-derived arbitrary
DMA address is dereferenced. QTD index is bounds-checked against the allocated
list. QTD reads are volatile and non-destructive; hardware can still update them
while sampled, so multiword descriptor/register values are not an atomic DMA
snapshot. These hooks are restricted to ESP32-S3 internal coherent memory.

Event QTD fields describe the current HCD read buffer at the hook, which can be
a different buffer from a just-parsed URB under double buffering. The raw QTD
status word must be interpreted with that distinction. Root ISR entries update
counters without filling the event ring with unrelated/SOF interrupts.

Timestamps `at` and `last-isr` in TRACE records are unsigned microseconds since
boot, wrapping about every 71.6 minutes. Ordinary DEVICE/BUS timestamps remain
milliseconds. HFNUM includes both frame number and frame-remaining bits. Event
rings are printed oldest to newest; counters wrap modulo 2^32.

## Build and review

    pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project usb-isolation
    pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project hid-generator
    python scripts/test-stress.py
    python scripts/package-stress-kit.py --usb-isolation --generator-trace

CMake generates an instrumented copy of hcd_dwc.c in the build directory.
`instrument_hcd.py` checks the exact reviewed upstream SHA-256 and unique hook
anchors; any source mismatch stops configuration. Managed component files stay
unmodified. The generated file replaces the original HCD compilation unit; it
does not add a second implementation. Patch review can compare the generated
file with managed_components/espressif__usb/src/hcd_dwc.c. The overlay and trace
adapter are linked only into this diagnostic project.

For a control build in an activated ESP-IDF shell, configure with
`idf.py -D KMS_ISO_DRIVER_TRACE=OFF reconfigure`, then build. The default is ON.
With tracing OFF, the original HCD is compiled and the trace store is not linked.
Instrumentation adds fixed RAM and CPU work and can alter failure timing; a
quiet traced run does not prove a fix. Allocation/free lifetimes use the HCD's
existing lock. Lock order is HCD then trace; the serial dump never holds either
lock while writing UART. Printing happens only after storage is frozen.

## Flash on Windows

Extract the package. From its root in PowerShell:

```powershell
$espPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $espPython -m serial.tools.list_ports -v
$sparePort = 'COM3' # Select the current programming port for the spare board.
& $espPython -m esptool --chip esp32s3 --port $sparePort read-mac
```

Confirm that the reported MAC matches your spare board. Close other serial connections, then flash only
the spare host (no full backup required for this experiment):

```powershell
& $espPython -m esptool --chip esp32s3 --port $sparePort --baud 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 40m --flash-size 16MB 0x0 firmware/usb-isolation-bootloader.bin 0x8000 firmware/usb-isolation-partitions.bin 0x10000 firmware/kmswitcher_usb_isolation.bin
```

## Capture on Linux

Extract the included linux/ directory over the existing stress kit to install
runner 0.3.9. The instrumented identity includes `trace=1`; the runner requires
the frozen trace when that capability is advertised. It still accepts the recorded uninstrumented 1.5.0/1.6.0 images.
The new parser retains the first-fault trace as structured `driver_trace` data
in isolation-fault.json and as original lines in capture.jsonl. A 20-second
snapshot deadline allows the bounded trace dump at 115200 baud; a truncated or
malformed trace is reported rather than silently accepted. Older runners reject the extra capability field, so update before collecting
a new driver trace.

Power-cycle the spare host and use the same topology/seed/rate as the comparison:

    cd ~/KMSwitcher/stress/linux
    bash run-usb-isolation.sh --profile ~/.config/kmswitcher-stress/profile-one-generator.json --mode input --seconds 3600 --rate 30 --pattern stress

Only generator 1 should be enabled and its native USB connected for that profile.
Keep its UART on the laptop. For a direct run, first establish working VBUS and
native USB enumeration without a hub, and record the supply/wiring in fixture
notes. A hub run can also provide useful driver evidence; record the exact hub.
Save the complete results directory before restarting after a fault. Review
whether activation happened, QTD ownership changed, interrupts arrived, URBs
were dequeued, and callbacks ran. A driver behavior patch needs evidence of the
broken transition plus focused regression and hardware soak tests; this image
makes no claim to fix the underlying stall.

## Experimental frame-boundary workaround (runner 0.3.9)

The existing spare-master image adds a guard immediately before the HAL writes
channel enable, for interrupt IN channels only. It is enabled at boot and the
runner defaults to `--workaround true`. Pass `--workaround false` for a matched
control run using the same binary, tracing, hub, workload and rate:

```bash
bash run-usb-isolation.sh --profile ~/.config/kmswitcher-stress/profile-one-generator.json --mode input --seconds 14400 --rate 30 --pattern stress --workaround true
# Control, after restarting the fixture if the preceding run stalled:
bash run-usb-isolation.sh --profile ~/.config/kmswitcher-stress/profile-one-generator.json --mode input --seconds 14400 --rate 30 --pattern stress --workaround false
```

The guard avoids the first/last 20 microseconds of each 1 ms frame, deriving the
counter margin from HFIR. A 64 microsecond time budget plus a 2,048-read limit
bounds polling even if a frame clock or timer stops. It uses the HCD critical
section and a short nested stats lock; no allocation, sleep or UART output occurs
in the hook. If no safe phase is observed within the budget, it counts a timeout
and executes the original activation so ownership/SDK state is preserved. The
actual before/after-enable HFNUM bracket remains available. A crossing count
records any observed frame-number change across that write, including controls.

`iso workaround 0|1` selects the mode only while watching is off. `iso arm` clears
guard counters without changing the mode. The capability is advertised as
`workaround=1`; the runner verifies the applied state before starting generators,
and rejects a missing or changed advertised state. Firmware without the
capability can be tested with explicit `--workaround false`. Tracing-disabled
control builds also require false. Existing generator firmware 75af11561422 is
compatible and does not need another flash for this experiment.

Each status/fault snapshot includes `KMSISO WORKAROUND` with enabled mode,
guard/time budget, eligible activations, waits, timeouts, maximum observed wait,
and crossings. The first-fault snapshot retains these counters. `result.json`
also records the requested mode and captured diagnostics. A completed guarded
run with timeouts or crossings is classified LIMITED, not PASS. A pending read
armed before mode selection retains its earlier activation; the mode controls
future activations, and counter comparisons must allow that initial pending read.

Native tests exhaust 216,000 phase/mode/endpoint cases at the 48,000/6,000-tick
frame intervals, compare guarded/control activation in a model of the captured
FRREM=17 crossing, and check timeout, stopped-clock/timer, clock-wrap, scope and
mode-reset behavior. These validate the software guard; they do not emulate the
silicon defect or establish that the hardware stall is fixed. Repeated matched
hardware runs, including a direct connection once powered/enumerated, remain
necessary to validate the hypothesis.

## Batched diagnostics

Identify each test board and its UART bridge before using the diagnostic commands.
Use the generator and isolation firmware built from this checkout.
Save output locally outside the public source tree.
