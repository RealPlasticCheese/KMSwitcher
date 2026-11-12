# Stress Toolkit: prepare, update and test

Install **KMSwitcher Stress Toolkit** from the published `linux-stress/` packages
and open it from the application menu. No Python, pip, compiler, source checkout
or companion installation is required. **Prepare test rig** supplies wiring
instructions; **Firmware** flashes complete images and updates test firmware.
S2 receivers offer **Direct USB** and **OTA through controller**. Generators use
direct USB. Choose only the spare S3 intended as an input generator.

Use bundled firmware offline, import a newer **stress firmware ZIP**, or use
**Get updates**. Private Azure downloads use browser sign-in and ZIP import;
public HTTPS feeds download directly. See [installation](installing.md).

Open **Run stress tests**, discover devices, review assignments and save a profile.
Run Baseline with generator USB connected directly to Linux, then reconnect it to
the controller hub for Quick and longer tests. Browser/report exports remain
available. Close the test dashboard before firmware maintenance; both share a
hardware lock. Profiles and reports are preserved across upgrades.
Use the [physical rig acceptance procedures](#physical-rig-acceptance) for
everyday-mode recovery, real desktop and power transitions, and measured latency.
The packaged launcher also requests sleep inhibition while a test dashboard or
hardware session is open, and releases it when the session closes. If the desktop
cannot provide inhibition, the launcher reports that the machine must stay awake.

The remaining sections describe source/developer workflows and detailed fixtures.

## Automatic radio recovery regression

The native stress suite runs the production channel recovery policy through
ten simulated minutes of HID and management contention. The selected dongle
stays healthy while an inactive dongle has radio failures, leaves range and
returns. The checks cover transmitter priority, probe pacing, all seven slots
and isolation of the affected link. A second scenario checks that two
degraded links can still qualify during foreground load and that a late
dongle rejoins before the master keeps the candidate channel.

For a live RF check, use a dedicated multi-PC rig in everyday mode with
automatic recovery enabled on the Companion's **Radio** tab. The hardware
stress runner uses retained test mode, which defers disruptive recovery.
Run keyboard and mouse input and a Companion transfer, then take an inactive
laptop out of range and bring it back. Check input responsiveness, confirm that
the returning dongle finds the master, and verify that the master keeps the
current channel. On a bench where radio conditions can be reproduced, repeat
with sustained impairment on two links and with one reachable dongle. Capture
the master log and check the trial, rollback, reconnect and slave reboot guard
against the [radio settings](radio-channels.md#automatic-channel-recovery).

## Manual developer flashing of generator S3s

Run these in PowerShell. Connect one tester S3 at a time through its USB-to-UART connector.
1. List ports before and after plugging it in. The newly appearing COM port is the tester:
```python
$espPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
& $espPython -m serial.tools.list_ports -v
```
2. Set its port—replace COM12—and read its MAC to confirm its identity:
```python
$testerPort = 'COM12'
& $espPython -m esptool --chip esp32s3 --port $testerPort read-mac
```
Identify the actual test boards and record their MACs locally. Reading the MAC briefly resets the selected board.
3. Close any serial console using that port, then flash the packaged firmware:

```python
Set-Location '<extracted-toolkit-folder>'

& $espPython -m esptool --chip esp32s3 --port $testerPort --baud 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-freq 40m --flash-size 16MB `
  0x0 firmware/generator-bootloader.bin `
  0x8000 firmware/generator-partitions.bin `
  0x10000 firmware/kmswitcher_hid_generator.bin
  ```
Repeat for the second tester. Both use the same firmware; don’t flash this image onto your working S3 master.

# Linux hardware test controller

For a separate rig in the same room, use the [saved radio channel](radio-channels.md).
Pair and [OTA-update spare receivers](slave-management.md#updating-one-or-all-receivers)
on channel 6 before changing the test rig's
channel. Channel changes are blocked while test mode is enabled.

The Linux desktop and browser dashboards manage the master UART, one or two
generator S3 UARTs and any enabled subset of paired slave slots 1–7. Counts, chip identities, UART paths,
names, slots, rates, durations and output settings belong to a saved JSON profile.
There are no fixture MAC addresses or fixed slave lists in the application.
Discovery never runs a test or changes routing. The application verifies each
identity again before generating input, and never silently reassigns a slot.

## Wiring

- Connect each generator's **native USB** to the production master's input hub.
- Connect each generator's separate **USB-to-UART** connector to Linux.
- Connect the production master's **UART** to Linux.
- Connect each slave under test to Linux, directly or through its powered hub.

Two generators and six slaves use nine Linux USB connections. The master input
hub and Linux hub are on separate USB buses. More Linux ports do not expand the
master's two-input/eight-channel limit. Each generator combines keyboard, mouse
and consumer usages on one interrupt IN endpoint, leaving room for two sources.
Remove other inputs from the master's hub, or explicitly select the generators
using its existing USB-input controls. Preflight requires exactly the configured
generators to be active there.

## Build and prepare firmware on Windows

From the repository, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-stress-kit.ps1
```

This produces `build/KMSwitcher-stress-kit.zip` containing the Linux application,
instructions, generator binary and master/slave application binaries and hashes.
It builds only. It does not flash, pair, change routing or start a hardware test.

The generator image replaces the firmware on the two **spare S3s**. Both use the
same image; their chip MACs supply distinct native USB and UART-protocol identities.
Build/flash one explicitly through its UART, checking its port first:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project hid-generator -Action flash -Port COMn
```

Replace `COMn` with that board's actual port. To use wired download mode when
needed, follow the board's BOOT/RESET procedure. The standard master and slave
images now include software-controlled test support (`CONFIG_KMS_TEST_SUPPORT=y`).
They use `build/controller` and `build/receiver`; everyday use and tests share the
same firmware, partition layouts and pairing format. Update the master and participating slaves
with their matching `.bin`/`.json`; see
[Updating one or all receivers](slave-management.md#updating-one-or-all-receivers)
for the slave command-line workflow and [master updates](master-updates.md) for the S3.
The Linux app does not automatically flash hardware. Version 0.2.0 requires the
master's `test-mode` command; older diagnostic firmware must also be updated.
For an existing local configuration with test support disabled, build once with
`scripts/idf.ps1 -Project controller -ResetConfig` (and likewise for `receiver`).
The old `-TestSupport` switch still creates a separate output directory for
compatibility, but its session/startup behaviour is now identical.

Everyday operation leaves tracing and fault injection off. Arming a trace allocates
23,552 bytes of heap RAM per master/slave; `bench off`/receiver disarm releases it.
Small summaries and larger TinyUSB control buffers remain in the firmware; the
keyboard/mouse input reports and radio HID wire protocol remain unchanged. The
slave controls interface adds feature report 126, with no new endpoint or CDC
interface. Active instrumentation can change throughput; compare results with
everyday operation and validate the firmware on physical hardware.

## Everyday and test modes

Outside a test session, the master automatically selects its saved boot dongle,
including Computer 2, or the first available dongle in Automatic mode. Normal
retained history and recovery stay enabled.

The test app stops the identified generators, then sends `test-mode on` before
selecting any receiver. The master pauses routing and commits a small session
marker to NVS before acknowledging the command. That marker survives power loss,
firmware updates and restarts: an interrupted test starts paused with capture and
fault injection disarmed. It never automatically selects the everyday boot dongle.
An unreadable marker also starts paused. Repeating the mode command does not
rewrite NVS; arming/freezing individual segments never writes the marker.

Console commands (also usable in the browser serial terminal):

- `test-mode`: show `KMSTEST MODE everyday` or `KMSTEST MODE test`.
- `test-mode on`: enter a retained test session and pause routing.
- `bench arm N`, `bench freeze`, `bench off`: arm/freeze/release a RAM trace.
  The master must already be in test mode. `bench off` clears injected faults
  but keeps the retained test session guard.
- `test-mode off`: clear the guard, injected master faults and trace memory.
  Routing stays paused until an explicit selection/resume; the next ordinary
  startup uses the saved boot dongle again.

During a test session, automatic diagnostic persistence is limited to one history
write per minute per board. Detailed reports and full counters continue in RAM
and on the test computer. Master records suppressed by this limit are not flushed
as a burst on exit; a receiver saves the latest journal when its budget permits.
The master session marker adds one write on entry and one on exit. Explicit
history clearing and firmware/settings changes still write flash normally.

## Install and start on Linux

Copy/extract the zip on the laptop. In its `linux` directory run:

```bash
bash install.sh
bash run.sh
```

The installer uses a virtual environment, installs the Python dependencies and
adds narrowly scoped udev permissions for KMSwitcher receivers and common UART
bridges. On Debian/Ubuntu it installs the required Python/Tk/build prerequisites.
Other distributions need Python 3.10+, tkinter, venv, development headers and a C
compiler. Internet access and one administrator setup step are required. Run the
app subsequently from the **KMSwitcher Test** application-menu entry or `run.sh`.
Keep the extracted kit in place because that entry points to its launcher. Run the
app as the normal logged-in desktop user. Reconnect devices if permissions have
not refreshed. Unusual UART bridge types may need an additional device rule.

`run.sh` uses the distribution's `systemd-inhibit`, when available, to prevent
sleep/idle suspension while the controller is open. On other systems it reports
that sleep inhibition is unavailable. It does not change permanent power settings.

Click **Discover**, review names and enabled devices, then **Save profile**.
The app reads each board's firmware identity; matching a UART bridge VID alone
does not establish its role. The master supplies slot-to-slave USB identities.
Newly discovered slaves present on Linux are enabled; absent ones remain listed
but disabled. For the initial fixture, enable 3–6 and enable 1–2 when connected.
Existing saved assignments are retained and mismatches require review.

Use Ctrl-click or Shift-click to select several rows, then **Enable selected**
or **Disable selected** to apply one state to the whole selection. Ctrl+A selects
all rows; the master remains enabled. Selections stay highlighted after a bulk
change. Editing and removing require exactly one selected row.

Discovery keeps each UART open while retrying the read-only identity request
up to four times (six seconds). The progress tab shows each port, attempt and
received UART line. Its final summary distinguishes an unidentified master
from other unavailable ports. A failure to query pairings does not discard a
successfully identified master. **Copy diagnostics** copies the app version,
environment, connection messages and any UI traceback without a terminal.

For a Linux app update, `python scripts/package-stress-kit.py --app-only` creates
`build/KMSwitcher-linux-app.zip` without requiring or including firmware. Close
the Linux app and extract it over the original kit folder, then reopen the same
application-menu entry. Confirm version 0.3.1 in the title. Saved profiles remain
in the home directory. The browser dashboard needs no new Python dependencies;
hardware tests require the matching master/receiver firmware with software test
mode described above.

Select a preset in **Test setup**, then **Start test**. Preflight opens all
configured receiver HID interfaces and exclusively grabs their associated Linux
input devices before arming a generator. An unavailable receiver, missing feature
protocol, incorrect pairing identity or missing input grant blocks the run.
UARTs open with DTR/RTS deasserted. No pairing/settings reset is performed.
The master starts **paused while a test session is retained**, including after an
unexpected reboot, so generated input cannot automatically return to a normal-use
slave outside the captured fixture. The app detects its boot notice and stops that run.

**Stop** and window close request orderly cleanup. Generators also have a
two-second control heartbeat timeout. The master is left **paused** after a run;
use its normal Resume/select controls when returning to everyday use. A successful
or cancelled run clears test mode only after generator neutral completion and
receiver cleanup succeed, restoring the usual startup preference. A failed or
interrupted run keeps the guard; stop/disconnect generators and send `test-mode off`
when ready to return to everyday startup. Captures
are closed and input grabs released after stop/release traffic has settled.
Input grabs exist only while the process holds them; use this laptop as the
test target, not for consequential typing/clicking during fault experiments.

## Presets and what they establish

### USB-stall campaign

Version 0.3.13 freezes all trace windows when the host first observes a stream
mismatch or device fault, including with stop-on-failure disabled and during
overload. Input, recovery, capture and summary counters continue to the configured
deadline; freezing does not end the segment. This preserves the available detail
window without waiting for cleanup. Host polling cannot recover records already
overwritten before detection. Firmware with local first-fault freezing adds
protection when the host is delayed.

Version 0.3.3 tolerates delayed fault/status output while continuing generator
heartbeats and cancellation. The [USB-only isolation experiment](usb-isolation.md)
uses separate firmware on the spare master to test the SDK/USB path without
the switcher's parser, routing or radio. It uses the same generator firmware.

Version 0.3.2 fixes false inactive-generator failures when alternating sources:
generator configuration retains prior counters until that source starts. The
runner records an idle baseline and checks for changes, including CRC and missed
reports. A non-neutral idle source blocks input before either source starts.

Failed attempts require recovery before both retries and the next recipe. A
pending generator triggers at most one isolated host reconnect even when the
host already reports healthy inputs. Failed neutral completion blocks further
cases, with the reason and remaining case count saved in the summary.

Update **both tester S3s** with the generator firmware accompanying 0.3.2. A host
bus reset can cancel their pending report without a device-detached callback;
the firmware now retires it on reconfiguration and sends a neutral report. It
does not count the cancelled report as completed or resume the failed workload.
Existing master and S2 firmware supports these app changes. The app-only update
also works with earlier generator firmware, but cannot fix that firmware's
pending-transfer bug; it stops rather than continuing if recovery fails.

The recorded October 21 synthetic-generator incident is retained in
`linux/tests/fixtures/usb-campaign-2026-10-21.json`. It reproduced the host's
completion-interrupt stall without Logitech hardware or RX-FIFO status set.
Reports distinguish `host-no-completion` from `host-no-completion-rx-fifo`;
both require advancing frames, DMA/global interrupts enabled, no channel IRQ
progress, owned descriptors, pending controls and a powered, connected port.
Neither classification establishes the underlying hardware or driver cause.

Version 0.3.1 also recognizes SDK endpoint-allocation failures and counts repeated
messages without filling the fault window. The separate offline
[K830 hot-plug matrix](k830-hotplug-matrix.md) exercises allocation/retry scenarios
and selected real SDK allocation functions. The live campaign does not physically
unplug individual inputs or toggle a hub's manual switches.

Version 0.3.0 adds the `usb-stall` preset. Use the isolated Linux generator fixture
and matching updated S3 firmware; the preset requires `usb-status <ID>` framing.
Older firmware fails the check before traffic begins. Existing S2 test firmware
and generator firmware need no protocol changes.

Select enabled receivers, rates and segment duration, then choose `usb-stall`.
The finite matrix runs generator 1 alone, generator 2 alone and both (where
configured), at every selected rate and destination. Each combination covers
mixed input, idle then input, changing keyboard LEDs, LED plus slave-status
traffic, extra console polling, pointer input, media input and receiver-readiness
rejection. Disabled devices are excluded. The readiness override keeps physical
USB connected; it does not simulate a Windows reboot.

**Campaign idle period** applies before idle/wake cases. **Campaign attempts** is
the maximum attempts for a failing recipe (default 3, maximum 10); a healthy
case runs once. **Fixture / hub notes** records details such as hub model, port
switches and radio placement. The plan is capped at 1,024 cases. The preview
shows case count, initial active/idle duration and repeat limit; switching,
trace downloads and recovery add time. The normal soak duration does not bound
this finite campaign.

Every attempt saves correlated before/after USB state, stream counters and timing
maxima. Periodic samples run every five seconds during input (every second for
the console-load case). The collector caches automatically printed fault windows,
then requests the frozen fault and all available trace windows on failure.
It avoids the full master `history` dump. Evidence has the configured disk budget;
exhaustion, missing frames, reset or detached capture cannot pass.

Before retrying a failure, the runner pauses, clears test fault/readiness/LED
overrides, checks input health, requests at most one USB-host reconnect if needed,
then verifies generator neutral completion and the exact USB inventory. It does
not reset boards, power-cycle PCs or automatically re-arm a detached capture.
If recovery fails, the report lists the remaining attempts as not run. With
stop-on-failure enabled, the campaign stops after that failing case's repeats.

Reports include `campaign-plan.json`, `campaign-summary.json`, per-attempt
`usb-*.json`, `recipe-<case ID>.json`, the profile, environment/app version and
normal UART/HID/trace evidence. Download the complete report ZIP to retain these.
The summary counts failure signatures per recipe and flags repeated classified
symptoms. Unclassified failures do not establish reproduction of the same stall.
The host live-frame/RX-FIFO/QTD signature is separate from receiver-link events,
input queue pressure, source completion stalls and missing evidence. All retain
an unconfirmed cause; a PC reboot is one possible receiver-side explanation.

Import a saved recipe with **Replay USB recipe** in either dashboard, or run:

```sh
python -m kms_stress --run --profile /path/to/profile.json --preset usb-stall
python -m kms_stress --run --profile /path/to/profile.json --replay-recipe /path/to/recipe-CASE.json
```

Replay uses the saved seed, source selection, rate, duration, activity and idle
period. Chip identities, generator ordering and receiver slot assignments must
match. Use **Clear campaign replay** to return to the complete matrix. A seed
repeats generated input, not USB/ISR/radio scheduling. Fixed generator descriptors,
short packets and polling intervals cannot establish real Logitech/RF behaviour,
other hubs, exact-MPS variants, physical power faults or Windows compatibility.
The matrix allows up to 1,200 cases, including all seven default rates with two
generators and seven receivers (1,176 cases).

### All tests — timed loop

App 0.3.11 adds **All tests — timed loop** (`all-tests` in the CLI/Tk dashboard).
Set **Total duration** to `28800` for eight hours and clear **Stop at first
unexpected failure** to keep testing after a recorded failure. The browser
clears that checkbox when you select this preset. **Recovery attempts** defaults
to three. Existing profiles remain compatible; the duration uses the same saved
field as Soak. No firmware update is required beyond the normal test-capable
master/receiver images.

The loop interleaves everyday input, pointer, media, overlap, resilience,
background activity, every directed receiver pair, continuous handover, rate
sweeps, overload followed immediately by a low-rate check, and the USB-stall
matrix. The matrix includes either generator alone and both together, idle/wake,
LED feedback, background management, console diagnostics and receiver-readiness
overrides. Completed cycles repeat with the next deterministic seed. The report
lists unvisited cases and cases that have only partial attempts. A clean timed
run with incomplete coverage is `INCONCLUSIVE`. Any confirmed failures keep
their verdict. Counts are available by test, receiver and rate. Baseline needs the
generators connected directly to Linux and is excluded. Physical power cycling,
electrical hot-plug, real-device descriptors and Windows/BIOS still need separate
tests.

With one enabled generator, the overlap pattern still runs, but it cannot
exercise cross-source merging. Runner 0.3.18 records that as `overlap_pattern`
coverage and explicitly excludes simultaneous two-source input. The separate
`shared_key_overlap` flag requires a passing overlap case with two verified
sources and observed simultaneous holds of both the shared key and the shared
button. The result records evidence for each control separately.

The duration starts after preflight and includes workload, trace collection and
recovery time. At the deadline, no further cases start and active generation
stops. Pending serial commands, neutral releases, trace downloads and final
cleanup can finish afterward. A cutoff during generation marks that case as
partial. A directed live handover counts after fresh destination input and
the complete observation window. The dashboard shows the cycle, current case,
remaining time and result counts.

The browser shows remaining time and result counts above the fixture for every
test preset. Soak and All tests use their run timers, starting after hardware
preparation. Other presets show an estimate of the remaining scheduled input,
including campaign idle periods and live handovers. The estimate updates for
each segment and for additional USB campaign attempts; setup, trace downloads
and recovery can add time. The summary shows when final capture and cleanup
are running, then the final result. Reloading the page retains the countdown.
Progress and diagnostics lines carry the server's date, time and UTC offset;
downloaded logs retain those timestamps. Selecting page text temporarily holds
visual updates so it can be copied. Server polling and the test continue, and
the next poll displays the latest state after the selection is cleared.

Failures and capture errors remain in the report. Before continuing, recovery
verifies board identities, stops the generators, pauses routing, clears injected
faults and opens fresh receiver capture queues. It may reconnect the master's
isolated USB input bus when neutral completion or inventory checks fail. It never
flashes or deliberately resets a board. A changed identity, unavailable hardware,
exhausted recovery attempts or exhausted evidence budget ends the run rather than
claiming reliable capture. The usual retained test-mode guard remains after an
unsuccessful run.

Since runner 0.3.16, ordinary input segments also fail on device faults reported
by the master UART or retained device traces, even if their input streams match.
With **Stop at first unexpected failure** cleared, the timed loop retains the
failure and performs its bounded recovery before continuing. Explicit overload
cases retain their separate characterization and required recovery check.
Faults during route selection remain in the result, but an earlier fault does
not immediately freeze a newly armed trace. Overlap verification requires the
complete master arrival order; a fault-frozen partial window cannot establish
a merge failure. Missing reference evidence is inconclusive unless a device
fault independently establishes failure.

For an eight-hour CLI run with the saved profile:

```bash
cd ~/KMSwitcher/stress/linux
.venv/bin/python -m kms_stress --run --preset all-tests --duration-seconds 28800
```

Linux capture drains the grabbed input-event queues on a separate thread from
HID decoding and evidence writes, and bounds each HID read batch so one busy
interface cannot starve other receivers. Since 0.3.14, `SYN_DROPPED` in those
desktop-protection queues is counted in `capture_warnings` and the HTML report,
while the grabs remain held and draining continues. These evdev queues do not
provide verification data or reconstruct input state. The independent hidraw
streams, generator completion proofs and device checkpoints still determine the
verdict; missing or corrupted HID reports, read failures and disconnects still
invalidate a run. See the kernel's [SYN_DROPPED documentation](https://cdn.kernel.org/doc/html/latest/input/event-codes.html).

### Other presets

| Preset | Checks |
| --- | --- |
| Baseline | Connect generator native USB directly to Linux; verify tagged payloads, order, counts, CRCs and neutral release, without a master or slaves |
| Quick / Everyday | Per-source key/modifier/button/movement streams on every enabled slave |
| Pointer | Legal movement coalescing, positive/negative axis totals, net scroll and button edges |
| Media | Both sources' distinct consumer-control presses and releases |
| Overlap | Both generators share A and the left mouse button; release behavior is checked against actual master ingress order |
| Routing | Every directed pair with neutral boundaries, followed by continuously active input and bounded old-route release/isolation checks |
| Resilience | Controlled input/ACK losses, retries/deduplication, selection with receiver USB not ready, and rejection of an unpaired slot when available |
| Background | Input during application status logging, slave-status traffic and selected-host keyboard LED feedback |
| Clipboard | Dongle-only synthetic file packets during verified input, lost receipts, cancellation and post-transfer recovery; requires two receivers |
| Limit | Configured rate sweep for every enabled slave, recording requested and achieved rates |
| Overload | Highest configured rate, followed by a low-rate recovery check on that slave |
| Soak | Repeated stress segments across enabled slaves at one selected rate for the configured overall duration |
| Mixed soak | One uninterrupted window per enabled receiver after a configurable idle, with typing bursts and continuous pointer input at independent rates |
| Full | Everyday, overlap, resilience, background, media and rate sweeps; then continuous handover safety |
| USB-stall | Finite source/rate/activity matrix, framed USB evidence and bounded repeats of failing recipes |

**Resilience** requires nonzero injection counters for both master input drops
and receiver ACK drops. At low report rates the runner extends the minimum
duration to allow enough opportunities for both injections. Missing injection
evidence leaves an otherwise clean case inconclusive.

The **Background** preset also saves live receiver radio snapshots before and
after input verification when the S2 advertises diagnostic capability bit 32.
It fails if the receiver restarts or its failed-MAC-completion counter increases.
Older S2 firmware is explicitly marked unsupported for this additional evidence.
The probe performs read-only management operations and checks that automatic
receiver restarts are disabled or deferred by test mode, so recovery cannot hide
an injected fault. It retains the existing cache/update-guard checks.

Offline native tests exercise positive restart scheduling, active/inactive
thresholds, clock wrap, cooldowns and rolling budgets, persisted budgets across
S3 restart, stale queued commands, update/pairing/sleep guards, route restoration
and actual S2 journal persistence/storage failures. Run
`python scripts/ci-tests.py native` and `python scripts/test-stress.py`.
These do not establish recovery timing on physical hardware; controlled hardware
acceptance of automatic restarts remains a separate test after updating both roles.

### Continuous mixed soak

Install current HID generator firmware on both generator boards. Enable both
generators, select **Mixed soak**, and use the **Soak / All tests total, or
Mixed soak per receiver** field for the uninterrupted duration on each receiver.
The smallest configured rate drives Generator 1's typing bursts and the largest
drives Generator 2's continuous pointer, scroll and button activity. Both run
on their own schedule. **Campaign / Mixed soak idle** sets the quiet interval
before each receiver's workload starts.

For an eight-hour window on each enabled receiver, run

```sh
kms-stress --run --preset mixed-soak --duration-seconds 28800
```

Choose rates from the baseline and limit results for the fixture. Set a capture
budget with enough room for the whole run across all enabled receivers. The
report records each source's requested and achieved rate, idle interval, and
actual generation time. The verifier checks keyboard order and release, pointer
totals and button edges, and off-target input. A run under 95% of a requested
source rate is `LIMITED` when its other checks pass.

### Dongle-only clipboard regression

Select **Clipboard** in the dashboard, or run:

```sh
python -m kms_stress --run --profile /path/to/profile.json --preset clipboard
```

Enable at least two receivers and close their normal Companion processes while
the toolkit owns the test fixture. Each enabled receiver takes a turn receiving
synthetic file packets while the existing generator/input oracle checks keys,
buttons and movement. The toolkit uses the actual desktop/clipboard USB feature
reports and encrypted dongle relay; it opens no network connection and does not
read files or change a desktop clipboard. It establishes temporary helper
sessions, validates relayed bytes and offsets, finishes a small transfer, then
withholds DATA receipts during a second transfer to force retries before CANCEL.

Each transfer segment lasts at least 15 seconds and requires at least one second
of concurrent input monitoring after cancellation. A separate normal-input
segment follows immediately. Incomplete transfer/cancellation, corrupted data,
stalled helper heartbeats, input mismatches and device faults cannot pass. The
report records transfer progress and exports the latest 256 radio/clipboard
trace lines in `radio-diagnostics.json`, alongside the full bounded UART capture.
**All tests** includes this transfer/recovery pair when two receivers are enabled;
one-receiver rigs list it as excluded.

This tests firmware transport contention and recovery using synthetic companion
traffic. It does not test file packaging/extraction, OS clipboard integration or
an actual large-file transfer in the desktop app. Native tests separately cover
a missing transmit completion, SDK recovery failures and recovery limits; automatic
radio restart is intentionally deferred in hardware test mode so a failure is
preserved for diagnosis.

Six enabled slaves generate all 30 directed handovers. Input is routed to one
slave at a time; all other slaves are monitored for leakage. Neutral releases
are expected. During continuous switching, the old route has a 1.25-second
transition grace; input remaining held afterwards, or fresh input outside that
grace, fails. Switching intentionally discards old-route backlog, so this test
checks isolation/release rather than claiming lossless execution at the boundary.
It also requires fresh input on each selected receiver, valid generator
completion proofs and no device fault in UART or the handover trace.

Requested rates are **USB reports per second per generator**, not characters per
second. A generator records offered, submitted, USB-completed and missed counts,
plus a payload CRC. `LIMITED` means correct delivery at the achieved rate but less
than 95% of the requested rate. It does not certify the higher requested load.

The setup preview shows the actual rates per generator, combined offered rates,
enabled receiver slots, segment/soak durations and stop-on-failure setting. Soak
selects the middle **entry** in the configured rates list; it does not sweep it.
With `10,25,50,100,250,500,1000` that means 100 reports/sec per generator (200
combined with two generators). Set a single rate such as `30` to choose it
explicitly. This is workload selection, not a claim of supported throughput.

Requested duration and actual generation time are recorded separately. Achieved
rates use each generator's start-to-stop-command interval, excluding subsequent
release polling, trace downloads and cleanup. Slow stop-command acknowledgement
can extend another generator's interval; each source retains its own timing.

Normal suites require matching streams and released controls. An overload loss
can be labelled `OVERLOAD OBSERVED` only when the armed trace shows queue
saturation and all observed faults are consistent with that pressure. It also
requires complete source/evidence checks, no inactive-slave leakage and released
controls. The next low-rate segment must pass. Unexplained loss remains a failure. Capture loss,
reset/missing diagnostic evidence or failed cleanup is inconclusive.

A running generator with no completed-report progress for three seconds fails
the workload promptly, even if its UART still answers. The partial segment keeps
its run/configuration, counters, actual durations and available traces. Earlier
observed master faults are shown before later stop/release errors; all generators
are checked on shutdown. A confirmed workload stall or incomplete neutral release
is FAIL; unavailable evidence without that proof remains INCONCLUSIVE. Cancellation
also retains the active partial segment, labelled CANCELLED. These outcomes never
count as passing verification of a complete requested segment.

Current scope excludes power/sleep system-key generation, BIOS/Windows behavior,
automatic physical power switching, protocol-mode changes and RF jamming. A
test-only readiness override checks selection of a reachable USB-not-ready peer
while physical USB capture remains connected. Delivery waits for USB readiness;
selection does not. A separate unpaired-slot check verifies rejection without
changing the selected route, unless all seven slots are paired. These checks do
not simulate electrical disconnection. A
powered hub alone does not imply per-port software power control. Resets, physical
unplugging and controlled power interruptions can be exercised manually; the app
stops and preserves available evidence when capture disappears. It does not
automatically re-arm across a disconnect. Shortcut recognition remains covered
by the existing native suite; this harness selects destinations through UART.

## Evidence and failure localization

Each run writes its profile, UART/HID capture, per-segment results, rolling trace
windows, full-run summaries and `report.html` into its results directory. The
report is standalone HTML, with no server or external assets. Seeds regenerate
the workload; USB/radio scheduling will vary on rerun.

The independent Python oracle verifies completed generator patterns, ordered
per-source key/button/mouse/media streams and final released state. Everyday,
stress and media profiles deliberately change keyboard state each sample, so
their mouse reports cannot legitimately coalesce. Pointer-only mode uses signed
movement aggregates; it does not claim identical movement packet counts.

For global keyboard ordering, the master's diagnostic observer independently
merges the known test input bytes, hashes the resulting key stream and compares
it with router output. Slave USB completions and Linux capture produce the same
signature. Input payload counts/CRCs link each generator to master USB reception.
These checkpoints distinguish source/USB input, decoding/routing, radio/slave USB
and Linux collector boundaries. CRC/count checks are noncryptographic, not a
mathematical proof against every possible corruption.

A mismatching input/routed signature identifies a boundary, not necessarily a
HID decoding bug: recovery deliberately blocks held input. Reports flag this
qualification when faults/recovery are recorded, distinguish recovery notifications
from faults, and keep the earliest observed UART fault with its timestamp. They
do not infer the initiating cause from records that were overwritten.

The receiver retains an accepted input's acknowledgement if its transmitter is
busy, and retries submission before status/management traffic. This avoids waiting
for a master retransmission solely because background traffic occupied the radio.
Native contention tests verify ordering/deduplication and queue behavior; the actual
sustained rate still requires a fixture run. The master USB cleanup deadline and
retained restart faults are provided by the separate USB recovery implementation.

Detailed records are a bounded 256-entry rolling window. Normal overwrites do
not invalidate the full-run summaries, but earlier detailed events may be gone.
Updated master/receiver firmware automatically freezes its local window after
recording the first fault. The preceding 255 records and that fault cannot be
overwritten by later input, retries or recovery, even with stop-on-failure off.
Boot, selection and recovery notifications do not trigger the freeze, nor does
an injected packet drop by itself. Input, recovery, injection settings and
full-run summary counters continue normally. Rearming starts a fresh window;
manual freeze and existing trace readers remain compatible. No additional trace
RAM or flash writes are needed. Each board triggers independently, so this does
not establish synchronized capture or recover details already overwritten before
its first fault. The app also freezes windows on a live mismatch before dumping. A slave
reset loses its RAM trace; changed boot/run identifiers are reported, not hidden.
Diagnostic records are never persisted as keystroke history to firmware flash.
Device-local microsecond timestamps are not synchronized clocks. Reports do not
claim precise end-to-end latency from UART arrival times; capture timestamps and
local trace timing remain available for diagnosis.

The configured capture/trace budget stops a run before regular evidence files
exceed that allocation. Final JSON/HTML reports, exception details and one bounded
interrupted-trace snapshot are retained in addition to that budget. Leave free
disk space for those files; budget exhaustion never counts as a passing run.

The Linux adapters follow the kernel's [hidraw report and feature-transfer
format](https://www.kernel.org/doc/html/latest/hid/hidraw.html) and use
[python-evdev exclusive grabs](https://python-evdev.readthedocs.io/en/latest/apidoc.html)
on the configured fixture's input nodes.

## Restarting fixture devices

Each device row in the browser has a **Restart** button. **Restart all devices**
restarts every listed device, including devices unchecked in **Use**. Enable
**Restart enabled devices before each test** to restart only the participating
devices before preflight; direct-USB Baseline restarts its generators only. The
option is saved with the profile and also applies to headless runs. Older
profiles keep automatic restarting off until the option is enabled.

Restarting preserves firmware, pairings and saved settings. Sources stop and
master routing stays paused before any reset. The master and generators use
their UART reset lines; receivers use a USB command and need updated receiver
firmware. The tool checks identities and receiver firmware support before
resetting devices, then waits for them to return. Unsupported firmware or a
receiver that cannot service USB requests gives an explicit update/replug
message. Restart controls are unavailable during testing or another hardware
operation; **Stop** cancels preparation but an already requested restart still
finishes. Restarts happen before capture, never automatically during a test.

An unavailable receiver error names the configured device and suggests restarting
it. The original controller error remains in the report. Restarting cannot
power-cycle an attached PC or restore a device whose USB firmware is unresponsive;
those cases still need physical unplug/replug or a controllable power switch.

## Browser dashboard and report downloads

In the kit's `linux` directory, start the browser version with:

```bash
bash runweb.sh
```

Open the address printed in the terminal, by default `http://127.0.0.1:8765/`.
No URL token, login or session cookie is required, including after a server
restart. No test starts automatically. Keep the terminal open while using the app.

The browser dashboard uses the same discovery, saved JSON profile, presets,
preflight, capture and cleanup engine as the desktop dashboard. Discover devices,
review/edit their names, UART paths, identities, enabled states and slave slots,
then save the profile and start a test. Progress and UART diagnostics update while
the test runs. **Run demo** creates a clearly labelled hardware-free report.
Profiles can also be imported from or downloaded to the browser's computer.
Run only one controller against a fixture at a time.

The runner enters the retained software test mode before routing test input and
only restores everyday startup after successful source/receiver cleanup. Failed
or interrupted sessions keep their restart guard, as described under **Everyday
and test modes** above. These rules apply to browser tests and server shutdown too.

The **Saved reports** form lists completed run folders in the configured results
directory, including reports created by the desktop or headless app. Filter by
folder name, result or message. **View report** opens its standalone HTML summary;
**Download ZIP** downloads the entire run directory, including its profile,
`result.json`, `report.html`, UART/HID capture and trace files. The latest run also
has view/download buttons beside the test controls. Old reports remain available
after restarting the server. Incomplete folders are omitted until their final
report is written. Archives use temporary disk space on the server rather than
holding a whole capture in RAM, so allow space for the ZIP as well as the evidence.

Paths, device access and captures are on the Linux server. To browse a different
results folder, edit **Results directory on server** and **Save profile**. Downloaded
files go to the browser's normal download location. **Download log** saves the
current bounded progress/diagnostic log separately.

For access from another computer on a trusted LAN, explicitly listen on all IPv4
interfaces, optionally choosing a port/profile/output directory:

```bash
bash runweb.sh --host 0.0.0.0 --port 8765
bash runweb.sh --profile ~/bench/profile.json --output ~/bench/reports
```

Replace `127.0.0.1` in the printed address with the Linux server's LAN IP address.
Anyone who can reach that address can view results and control the rig. The server
uses HTTP and is intended for local or trusted-network use. Browser command
requests still require the dashboard header and a matching origin. It requires
no Node.js installation or external web assets.

Closing or reloading a tab does not stop a run: reopen the dashboard to reconnect
to its progress. **Stop** requests cancellation and waits for the existing runner
to stop generators, pause the master and release captures. Ctrl+C or SIGTERM in
the server terminal also requests cleanup and waits before exiting. The launcher
uses the same sleep inhibition as `run.sh` while the server remains open.

An existing desktop installation needs no new Python dependencies. For a new
machine that only needs the browser dashboard, install without Tk or a desktop
menu entry:

```bash
bash install.sh --web
bash runweb.sh
```

The ordinary `bash install.sh` still installs both the desktop prerequisites and
the browser server. Device-permission rules are the same for both interfaces;
run the server as the normal user with permission to access the attached fixture.

## Physical rig acceptance

Use the acceptance CLI for tests that need everyday-mode firmware, the installed
Companion, physical USB or power changes, or an external acquisition clock. It
generates procedures and evaluates observations and artifacts. The operator
performs the fixture actions using the real devices and apps. These commands
also run on a machine separate from the rig with the captured files.

```sh
kms-stress acceptance plan > evidence.json
kms-stress acceptance plan radio-recovery > radio-evidence.json
kms-stress acceptance check evidence.json --report acceptance-result.json
```

The full plan includes cross-peer contention latency, automatic RF recovery,
real shortcut and screen-edge switching, Windows/Linux power and BIOS, physical
USB hot-plug, real Companion clipboard transfers and uptime, continuous mixed
soak, and interrupted master/receiver OTA. The source checkout exposes the same
commands through `python -m kms_stress acceptance` with `linux` on the Python
module path.

1. Write the board and application builds, host OS versions, wiring, hub,
   receiver slots and radio setup in `fixture`.
2. Follow the generated `steps`. For every criterion, set its observation
   `status` to `PASS`, `FAIL` or `INCONCLUSIVE` and put the measured outcome
   and its evidence location in `note`.
3. List supporting files in `artifacts` using paths relative to the evidence
   JSON. For example, retain UART/status logs, host input captures, Companion
   logs and file hashes, or the firmware updater result. For Mixed soak, set
   `runner_result` to the Toolkit's `result.json`. The checker verifies that a
   complete mixed segment passed for each enabled receiver.
4. Run the evidence checker. It records SHA-256 for supporting files, retains
   explicit failures, and leaves missing checks, captures, or required files
   inconclusive. The CLI exits successfully when every selected case passes.

### Common-clock latency captures

Fill the latency limits in the plan before running. Use a shared acquisition
clock for the input trigger and all observed destinations, such as a
multi-channel USB or logic-analyzer capture with a single timebase. For keyboard
and pointer windows, use identifiable key and button edges during the workload.
For handover, measure from the switch trigger to the first fresh input at the
expected host. UART arrival time and independent host clocks cannot provide
these measurements.

Set each window's `path` to a JSON file with this structure.

```json
{
  "window": "contention-keyboard",
  "clock": "shared acquisition clock, nanoseconds",
  "samples": [
    {
      "sequence": 1,
      "sent_ns": 1000000000,
      "slot": 2,
      "observed": [{"slot": 2, "ns": 1005400000}]
    }
  ]
}
```

Include every source sequence, even when `observed` is empty. Keep duplicate
and wrong-destination observations in the capture. Use separate captures for
baseline keyboard, baseline pointer, contended keyboard, contended pointer,
and handover. Capture at least 100 source events per continuous window and
20 handovers. The report computes nearest-rank p95 and p99, worst-case latency
and the longest gap between deliveries during each continuous window. Each
measurement is compared with the corresponding limit in milliseconds.

## Headless operation and development checks

Save hardware results locally and review them before sharing.

It can run in an SSH session from PuTTY while the fixture stays connected to
Linux. To investigate a silent master UART, close the dashboard and run this
from the kit's `linux` directory, replacing the port with its actual by-id path:

```bash
.venv/bin/python -m serial.tools.miniterm --dtr 0 --rts 0 --echo /dev/serial/by-id/YOUR-MASTER-UART 115200
```

Type `bench info` and press Enter; `about` reads ordinary firmware information.
Exit with Ctrl+]. Do not open the app and a serial terminal on the same port.
The terminal is provided by the already installed pyserial dependency. It does
not require a Windows UI or moving the UART cable away from Linux.

Exit status zero means PASS (or a passing explicit demo); FAIL, LIMITED,
INCONCLUSIVE and CANCELLED are nonzero. Demo uses simulated devices and labels
its report accordingly. It never opens a UART or captures input.

On Windows, `python scripts/test-stress.py` additionally compiles the real C
generator pattern, production HID parser and trace ring into a freestanding test
DLL. Tests compare 10,000 C/Python workload cases and check the actual generator
descriptor, trace wrap/pagination/CRC, injected losses, merged-stream summaries,
identity validation, missing/duplicate/corrupt input, coalescing and reports.
Compilation and these simulated checks do not establish live Linux USB behavior;
perform the direct-generator baseline and a low-rate fixture run before stress.

For **Baseline**, move each participating generator's native USB cable from the
master hub to Linux, retaining its UART connection. Select Baseline and enable
the generator(s); no master/slave configuration is required for this preset.
After a successful baseline, move native USB back to the master hub before
running the other presets. Both stages are controlled by the same dashboard.
