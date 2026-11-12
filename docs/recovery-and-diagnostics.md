# Connection recovery and retained diagnostics

The S3 retains the selected computer through radio errors, receiver restart,
USB suspension/disconnection and receiver input-lease expiry. It retries that
computer automatically rather than clearing the selection or choosing another
available PC. Startup activation also retries while an S2's previous master
session lease is still active. The receiver's old-session protection is unchanged.

Each outage produces one warning of **five red flashes**, each 125 ms on and
125 ms off (1.25 seconds total). The LED then returns to its normal indication
(off by default), while recovery continues indefinitely. Additional fault reasons
and expired/overflowed input during the same outage do not restart the warning.
A later outage after recovery or a new destination selection can warn again.
Selecting an unavailable computer produces its own five-flash warning.

A new user selection or pairing command ends the previous route and discards
its buffered input. An unavailable selection still leaves the current route
in place.

The transmitter keeps at most one ESP-NOW frame in the driver and waits for its
MAC completion before submitting another. This prevents retries to a disconnected
receiver from queueing ahead of a new destination. A busy/rejected submission does
not start a new input/activation acknowledgement timer. MAC completion itself is
not an application acknowledgement; the existing ACK/status checks still apply.
The old destination's release has a bounded retry budget even if it cannot be
submitted, with the receiver's input lease as the final release fallback.

## Input during an outage

The controller keeps up to **256 input state changes** in RAM, for at most
**five seconds**. Keyboard, modifier, mouse-button, media and system-key changes
retain their order. Adjacent unsent pointer-only reports with identical held
state can combine their movement, wheel and horizontal scrolling.

The single in-flight packet remains immutable until acknowledged. Retries keep
its session/generation/sequence so the receiver's existing duplicate rejection
prevents a lost acknowledgement from duplicating an event. Receiver status can
also confirm that exact packet before recovery builds a new routing generation.
Delayed status older than a confirmed packet cannot reactivate an old route.

Updated S3/S2 firmware can carry up to four queued input states in one ordered
batch and acknowledge the batch once. The first input is still sent immediately;
batching only uses reports already waiting in the controller. Every key/button
transition and relative movement is retained. The S2 reserves room for the whole
batch before accepting it, and a retransmission cannot apply any sample twice.
An older S2 continues using the original single-input packets. Both roles must
be updated to gain batch throughput; pairing records do not change.
Compact batches store small held-key sets as usage lists and retain a full bitmap
for larger sets. The controller reduces the batch count when necessary to stay
within the radio's 250-byte payload limit, preserving every buffered input.

If a receiver's lease expired, a fresh activation starts with neutral output,
restores the previously acknowledged held state, and drains pending input in
order. Legacy replay waits ten milliseconds after an acknowledgement. Batched
replay keeps at least ten milliseconds between batch submissions, including the
radio round trip, so recovery can catch up while input continues.
The five-second limit includes time spent waiting to replay an old sample.

On overflow or expiry, the entire remaining backlog is discarded and recorded.
Input is suppressed for the remainder of that outage, and held keys/buttons are
blocked until physically released. Recovery itself continues. Suppressed mouse
movement cannot leak into a later input report.

Buffering cannot provide exactly-once execution across receiver power loss or
restart: its acceptance history is gone, and USB acknowledgement is not proof
that an application processed a report. All inputs in the in-flight packet whose
acceptance is ambiguous on receiver restart are discarded and counted; unsent input remains
eligible for bounded replay. Restoring held state after a lease release can
produce another key-down. Replay should be tested in an empty editor before
relying on it during a drag, shortcut or other consequential action.

The input backlog is never written to flash. Diagnostic storage contains event
codes and counters, not key values or pointer coordinates.

## Fault history and statistics

Delivery loss keeps local switching commands usable while blocking stale PC
output. See the [shutdown and switching regression matrix](shutdown-switching-tests.md)
for the tested timing, recovery and held-input scenarios.

Input completion callbacks copy and rearm reports, then return before HID
decoding or test tracing. The USB owner decodes a fixed eight-report ring outside
SDK callbacks. `KMSUSBSTREAM` status/fault lines retain queue pressure, processing
timings and recent long dispatch gaps. See the
[USB stall investigation](usb-stall-scenarios.md) for the upstream research,
before/after regressions, measurement definitions and hardware-validation limits.

USB input reconnection waits for old endpoint cancellation callbacks and releases
before claiming replacement interfaces. A temporary memory allocation failure
during discovery or claiming retires that receiver's input bundle, waits for its
resources to drain and for a one-second backoff (`KMS_USB_ALLOC_RETRY_MS`). The
other receiver continues running. Retries do not restart the S3 or disconnect the
USB root port; unplugging or disabling the waiting receiver cancels activation.
The first allocation failure is logged, without repeating that warning every
retry. Unsupported interfaces and non-allocation I/O failures retain their
existing error handling.

The offline USB host tests inject endpoint, parser and transfer allocation
failures, delayed unplug callbacks, repeated reconnects at the eight-channel
limit, persistent pressure, disable/unplug during retry and clock wrap. These
check ownership and retry behaviour, not the real heap's fragmentation pattern
or the controller's electrical timing.

Diagnostics run on the **S3** and work with existing protocol-v2 receivers.
The optional ordered-input extension requires updated firmware on both roles;
it does not require pairing again.

The selected receiver's contact timeout is refreshed by status broadcasts and
acknowledgements matching the current in-flight packet. Delayed status broadcasts
therefore do not report a receiver loss while it is acknowledging input. Stale,
duplicate or mismatched acknowledgements do not extend that timeout.

The last 1,024 events are retained, with a boot identifier, uptime, selected slot,
idle duration, event reason, detail and repeat count. Events include connection
failure/recovery, receiver restart/USB unavailability, input removal/error,
buffer loss and keyboard LED output failure. Boot records include ESP-IDF's
reset-reason code. Consecutive identical events from the same boot/slot/detail
are grouped for a configurable interval, default one minute.

Event storage is queued to a low-priority worker. The input task never waits
for an NVS write or a history dump. Flash operations can still pause cached code
on the chip; buffering and the existing input leases remain necessary. Failed
event writes leave RAM history intact and are retried after the repeat interval.
Storage errors never abort routing or erase NVS to hide a failure.

Statistics accumulate in RAM and checkpoint hourly by default. The last 72
checkpoints are retained across reboots. Each is cumulative within its boot and
includes runtime, idle time, longest inactivity, decoded report/activity counts,
radio transmit/receive/retry counts, fault counts by reason, recovery duration,
replayed/discarded buffered records, peak buffer depth and storage errors.
During an explicitly enabled [test session](stress-testing.md),
automatic history writes are limited to one per minute per board. RAM counters and
test-computer captures continue; some test events may not survive a power loss.
Active unresolved recovery duration is represented by its start event; the
recovery-duration statistic sums completed recovery episodes.

Idle means no changed physical input or pointer movement for five seconds;
holding a key or mouse button counts as active. Repeated unchanged wireless
reports do not restart the idle timer. Event idle duration is the time since
the last physical input change and is zero while input is held.

History uses the `kms_history` NVS namespace in its own 256 KiB `kms_diag`
partition, with fixed rotating keys. Pairing
keys and trigger preferences remain in `kmswitcher`. A fault can be lost if power
fails before its queued write finishes; statistics since the last checkpoint
are also lost on reboot. Event uptime uses the existing 32-bit millisecond clock
and wraps after roughly 49.7 days; snapshot runtime is 64-bit.

## Frozen USB input recovery

USB handles, callbacks, control requests and recovery belong to a dedicated
priority-5 task on CPU1. Completed input reports are copied and their USB reads
are resubmitted before decoding. Decoded samples pass through a bounded queue
to the application task, which alone changes routing, radio input and history.
Console commands cross a separate bounded queue; status and fault snapshots
are printed by a low-priority worker without holding the USB task's buffers.

Queued samples carry source generations and capture times. Unplug/replacement
invalidates old generations, and recovery or queue overflow invalidates the
backlog. Samples older than `KMS_USB_MAX_AGE_MS` are discarded. Lost input clears
held output and suppresses held keys/buttons until release instead of replaying
old input when the application catches up. USB polling continues if the
application queue fills. This separation addresses servicing delays introduced
by the USB discovery rewrite; sustained real use is still required to establish
whether it eliminates the intermittent controller stalls.

The master supervises its application-owned HID descriptor, protocol, keyboard
LED and device-health control transfers. A control transaction outstanding for
**one second** triggers recovery. Quiet keyboard/mouse interrupt endpoints have
no inactivity timeout: an idle keyboard is valid. Every active physical input
device also receives a standard USB device `GET_STATUS` probe about every two
seconds, using its existing control endpoint and no additional host channel.
Two consecutive failed probes also trigger recovery. Probes continue when
keyboard LED forwarding is disabled.

The one-second threshold is deliberately conservative. A five-minute measurement
on the attached Logitech `046d:c534` receiver found descriptor/protocol callbacks
within 4 ms and 173 successful LED callbacks within 8 ms. Reconnect testing found
descriptor callbacks within 7 ms, protocol within 5 ms and probes within 7 ms.
These are observations on this setup, not guaranteed maxima for every device.
The existing 250 ms LED warning remains separate from recovery. Timing limits
are configurable in `config/kms_settings.h`.

Recovery releases held output, clears stale queued input and starts a fresh
radio generation on the same selected PC. It stops new USB submissions and
logically disconnects the root port. Both USB clients keep their transfer
buffers and device references until the SDK delivers completion/cancellation
callbacks. After all references and host devices drain, and at least 100 ms
with the root port disabled, it enables the port and rediscovers the inputs.
Success requires the original active-interface count and successful health
probes. Initial held keys/buttons and movement after reconnection are suppressed;
held state stays blocked until released. Pairings, input choices and preferences
are retained.

Quiescing does not halt endpoints on the stalled bus: the root disconnect
cancels them. The SDK's endpoint-halt call can wait indefinitely, so invoking
it before disconnect could block the input loop before its recovery deadline.
An independent ESP timer task now checks each USB input-loop call every 100 ms.
If a call exceeds three seconds, it records an RTC fault and restarts the S3
without needing another input-loop tick. It shares the existing one-restart
allowance, destination restoration and update/pairing exclusions.

Reconnect history is queued before cleanup starts. A blocked-call fault is
captured in RTC memory at the deadline without taking USB, UART or history
locks; after the software restart it is copied into retained history with the
original boot ID, uptime, idle duration, selected slot and operation stage.
RTC cannot survive a power loss before that copy. Stages are 1=polling,
2=quiescing, 3=root disconnect, 4=cleanup, 5=root reconnect, 6=rediscovery.

Cleanup has a three-second deadline; input rediscovery has an eight-second
deadline. At most three root reconnects are attempted in a ten-minute window.
Cleanup/API failure permits one last-resort S3 software restart, with an RTC
guard carrying the current destination or paused state across that restart.
A second cleanup failure stops automatic recovery. Ten minutes of healthy USB
operation replenish the restart allowance. A hub that is completely absent
after reconnection is left awaiting power/cable reconnection, without a reboot
loop. Recovery is deferred during master firmware updates, unconfirmed OTA boots
and pairing.

`status` shows `usb-recovery` state, attempts, completed recoveries and restart
allowance, plus per-operation completion/failure counts, successful callback
maxima and latency histogram buckets. History prints USB control deadline,
reconnect, recovered, blocked, restart and hub-absent events. The persisted
event/statistics layout remains compatible: these use the existing input-error
counter with tagged details `0x55PPAAII` (phase 1-6, USB address, interface).
Phase 7 is `USB call blocked`; its AA byte is the operation stage and II is zero.
The separate radio-recovery counters keep their existing meaning.

`usb-recover` queues a manual reconnect using the same safe cleanup sequence.

### Evidence captured before a USB reconnect

Each recovery episode freezes a bounded snapshot before quiescing inputs or disconnecting
the root port. `KMSUSBFAULT BEGIN` / `END` delimit its asynchronous UART output.
Run `usb-fault` to print the latest snapshot again. This records transport metadata
only, without keyboard reports, key values or mouse coordinates:

- The boot, uptime, selected computer and triggering operation/address/interface.
- All pending control requests, their eight setup bytes, expected lengths and
  submission times; the eight most recent completions or rejected submissions,
  including exact SDK status, actual/expected bytes and completion times.
- Cached hub/receiver VID/PID, parent/port/speed and input endpoint submission,
  last completion/status/length, pending state and report counters.
- Read-only S3 DWC root-port, frame, interrupt/mask and all eight host-channel
  characteristic/interrupt/mask/transfer-size registers. Frame progress is sampled
  at most once per 20 ms by the input task. No FIFO/pop registers are read, no
  interrupt bits are cleared, and capture submits no additional USB requests.

Operation numbers are descriptor=0, protocol=1, LED=2, health probe=3, and
manual/rediscovery retry=255. For completed transfers, SDK status numbers are
completed=0, error=1, timed out=2, canceled=3, stall=4, overflow=5, skipped=6 and
no device=7. `rejected=1` instead identifies an ESP submission error code.
A completed status with fewer than the expected bytes is retained separately.
Recent entries are oldest first. Input `seen=0` means that endpoint has not
completed a transfer; silence on an idle interrupt endpoint is not itself a fault.

Capture precedes recovery cancellation, preventing errors caused by our own
disconnect from being mistaken for the original failure. Printing runs on the
existing background console worker with nonblocking queue submission. If that
queue is busy, the latest snapshot remains available through `usb-fault`.
The first snapshot survives reconnect retries within that outage. It is RAM-only,
replaced on the next recovery episode and lost on reboot.
Save UART output if several full episodes need to be compared.

The snapshot also freezes a `KMSUSBTRACE` block from passive hooks in the pinned
ESP-IDF 6.1 / espressif USB 1.6.0 driver. The hooks call the original driver with
the original arguments and return its result. They retain raw interrupt flags
before the driver's normal acknowledgement clears them; they do not acknowledge
interrupts, change scheduling, send probes or copy HID payloads. Hook storage is
fixed internal RAM, protected by a short critical section; printing happens only
from the existing console worker. SDK upgrades require a hook review and build
guard update.
`usb-trace` prints a current trace through the same console worker without
triggering recovery or replacing the frozen fault snapshot.

The trace includes cumulative start/interrupt/error counts and last timestamps
for each of the eight hardware channels, plus the last error's raw flags and
endpoint characteristics. Healthy high-rate interrupt input is counted without
entering the 48-event ring. The ring retains control channel starts/interrupts,
channel errors, root-port changes and hub/port `GET_STATUS` results already read
by the driver. Events print oldest first with millisecond uptime; the ring is a
bounded event history, not a guaranteed number of seconds. Error counters and
last-error metadata survive ring wrap within the same boot. Channel counters
accumulate across channel reuse; `error-char` identifies the endpoint at that
error, while `char` identifies its latest activity.

| Trace kind | Fields |
| --- | --- |
| 1: channel start | `char`=HCCHAR, `bits`=HCINT, `size`=HCTSIZ, `flags`=HAL channel flags; `extra`=descriptor count, `detail`=starting descriptor index |
| 2: channel interrupt | Same raw registers captured before acknowledgement; `detail`=HAL result (0=complete, 1=error, 2=requested halt, 3=internal processing) |
| 3: root event | `char`=HPRT, `bits`=GINTSTS; `detail`=HAL port event (2=connect, 3=disconnect, 4=enabled, 5=disabled, 6=overcurrent, 7=overcurrent cleared, 8=remote wakeup) |
| 4: hub status | `detail`=port index (0 for hub), `flags`=request type (a3=port, a0=hub), `bits`=SDK transfer status, `size`=actual bytes including setup; `extra`=little-endian status word followed by change word, valid only for successful responses of at least 12 bytes |
| 5: channel freed | Last raw channel registers before release; removes the cached pipe identity |

For channel events, HCCHAR bits 22..28 identify the device address, bits 11..14
the endpoint number, bit 15 the IN direction and bits 18..19 the transfer type.
Raw HCINT can preserve a STALL, transaction error, babble or buffer error that
would otherwise be cleared before application fault capture. A control start
shows that the driver handed work to hardware; application pending time with no
matching hardware start instead points toward a queue/driver delay. Root IRQ
timestamps and per-channel activity help distinguish a general interrupt stall
from one stalled endpoint. These are clues, not proof of a faulty component.

### DMA and scheduler progress

`usb-fault` freezes a `KMSUSBSTATE` block alongside the interrupt trace.
`usb-trace` prints the same state from the running controller without triggering
recovery. The current register capture is supplemented by six samples taken at
most once per 500 ms from the existing input-loop tick, normally covering the
preceding 2.5 seconds. Samples print oldest first with their uptime and age.
Gaps show when that tick did not sample; this is not an independent hardware
clock or a guarantee of exact 500 ms sampling during flash/task delays.

| Printed field | Hardware state |
| --- | --- |
| `ahb`, `usb`, `reset` | GAHBCFG, GUSBCFG, GRSTCTL: DMA/global interrupt enables, USB configuration, DMA-request and AHB-idle/reset state |
| `nptx`, `ptx` | GNPTXSTS and HPTXSTS: non-periodic and periodic transmit FIFO/request-queue status |
| `host`, `interval`, `frame-list` | HCFG, HFIR, HFLBADDR: descriptor DMA/periodic scheduling, frame timing and scheduling-list base |
| `clock`, `channel-mask` | PCGCCTL and HAINTMSK: PHY/host clock gating or sleep and host-channel interrupt enables |
| `descriptor`, `buffer` | HCDMA and HCDMAB for each channel: current descriptor index/list address and DMA buffer address |
| Sample `core`, `mask`, `channels`, `irqs`, `last-irq` | GINTSTS, GINTMSK, HAINT, cumulative driver ISR count and last driver ISR uptime |

Each sample includes all eight channels' HCCHAR, HCTSIZ, HCDMA and HCDMAB so
address changes, DMA-index movement and transfer progress can be compared with
USB frame progress. HCDMA's low index is bits 3..8; its list base is bits 9..31.
GAHBCFG bit 0 enables global interrupts and bit 5 enables DMA. GRSTCTL bits
30/31 report a DMA request/AHB idle. HCFG bit 23 enables descriptor DMA and bit
26 periodic scheduling. PCGCCTL bits 0/1 stop PHY/gate host clocks; bit 6 reports
PHY sleep. FIFO status registers are read normally; FIFO data/pop registers
are never accessed. These states can be clues even when no interrupt was raised.

`KMSUSBTRACE QTD` records the three status words of an allocated control
channel's SETUP/data/status DMA descriptors. `submitted` is captured before
the real channel activation, and `current` at snapshot time. `base`, `count`
and `start` identify the list, the reviewed three-descriptor layout and the
stage last activated (0=SETUP, 1=data, 2=status). A descriptor's bit 31 means
hardware still owns it; after that bit clears, bits 28..29 hold success=0,
packet-error=1 or AHB/buffer-error=3. Bits 0..16 are remaining bytes. Ownership
and size changes are evidence of DMA progress, not proof of bus delivery.

Only the descriptor status words are copied, never the referenced data buffer.
Pointers come from the normal control-channel activation hook, not a raw
register address. The pinned HCD frees channels before descriptor storage;
the channel-free hook clears the cached descriptor pointer under the same
lock used by capture before that storage can be freed. Other list shapes and
interrupt-input descriptors are excluded. Capture does not use the SDK's
descriptor parse helper because it clears status. DMA/driver writes can race
individual reads, so a capture is a time-local observation rather than an atomic
bus transaction. Full frozen snapshot size is 5,580 bytes; it remains RAM-only
and is replaced on the next recovery episode.

Hub responses are captured only when the driver already requests them, so a
missing hub event cannot establish healthy power or wiring. This trace cannot
measure VBUS voltage, electrical signal quality or retries hidden inside the
USB controller. Proving a power dip or signal fault still needs measurement or
a hardware substitution. Trace data shares the fault snapshot's RAM-only lifetime.

Compact fields are also queued to the existing persistent history ring without
changing its record/statistics layouts. Detail `0x56FFVVVV` uses `FF` for the
field and `VVVV` for its 16-bit value: 1=operation/address, 2=address/interface,
3=pending control count, 4/5=root-port low/high words, 6=frame age in ms
(saturated at 65,535), 7=request type/request, 8=request value, 9=request index,
10/11=receiver VID/PID. Paired bytes put the first named value in the high byte.
Request/identity fields appear when available. These entries share normal ring
capacity and the test-session history-write budget; queued writes may be lost
if power or reset occurs before the background worker persists them.

These samples can distinguish pending requests, reported transfer errors and
observed host/frame state. SDK error status combines causes such as no response
and CRC errors; the ISR may already have cleared channel interrupt bits. A clean
register snapshot cannot exclude an electrical fault. Hub supply voltage and
signal integrity still require external measurement or a controlled hub/cable
comparison.
It resets the root-reconnect attempt counter, but does not bypass the one-restart
guard. It returns `busy` during recovery, updates or pairing. A blocked state
requires checking hub power/cabling and an explicit retry or S3 restart.

The SDK root-port operation resets the USB connection; it does not switch the
external hub's DC supply. A hub requiring an actual power cycle can still need
physical intervention.

To exercise real reconnects using the ESP-IDF Python environment:

```powershell
python scripts/test-usb-recovery.py --port COM8 --cycles 5 --min-inputs 4 --output build/usb-recovery-bench.json
```

This intentionally interrupts input during each reconnect. It verifies the same
S3 boot, selected PC and active-interface count after each recovery, and saves a
JSON report plus UART log. The tested two-combo setup normally has four active
HID interfaces; set `--min-inputs 2` when testing one combo. It injects no keyboard
or mouse input. Run `python scripts/test-usb-host.py` for deterministic missing/
late callback, cancellation, idle, wraparound, recovery-limit and held-input tests.

Controller builds with test support expose `usb-test-block` after an explicit
`test-mode on`. This deliberately suspends the input task during recovery so
the real timer must restart the S3 and recover the RTC history record. It refuses
in everyday mode, when recovery is unavailable or when the restart allowance is
used. Leaving test mode cancels a queued freeze. Test sessions remain paused
after reboot; select a destination explicitly if required. Minimal builds with
`CONFIG_KMS_TEST_SUPPORT` disabled omit the command.

## Controller console

| Command | Result |
| --- | --- |
| `help` | Describe every available command, slot ranges and pairing instructions |
| `about` | Master role, application/version, compile time, ESP-IDF version, ELF SHA-256, radio protocol/channel and slot capacity |
| `status` | Route/recovery/buffer state, current counters, idle time and reset reason |
| `key-trace` | Export the most recent 512 key-transition trace records from RAM; missing/unconfirmed key warnings are automatic |
| `usb-recover` | Queue a bounded USB root reconnect and input rediscovery; retain the selected PC |
| `usb-fault` | Print the latest pre-recovery USB request, device and controller snapshot |
| `history [X [Y]]` | All retained history; last X minutes; or entries aged X to Y minutes (ranges cover the current boot) |
| `stats-save` | Queue an extra statistics checkpoint without waiting for the hourly interval |
| `trigger-default` | Restore the configured default double-tap key while retaining pairings |
| `pair N` | Open enrollment for slot 1-7 for 30 seconds; routing pauses, then hold the running receiver's button for three seconds |
| `select N` | Select an available paired computer in slot 1-7 |
| `slaves` | Paired receiver contact times, cached build identities and health summaries |
| `slave-status N` | Fetch a receiver's current identity, counters and retention state |
| `slave-history N` | Download, verify and display that receiver's retained history |
| `slave-history-clear N` | Clear retained receiver history while preserving live counters and pairing |
| `prefs-get ID` / `prefs-set ID REV HEX` / `prefs-default ID REV` | Read/save/reset everyday preferences; see [the protocol](configuration.md#console-protocol) |
| `master HEX` | Local master identity/OTA endpoint; see [master updates](master-updates.md) |
| `pause` / `resume` | Release and pause input / restore the prior route if available |
| `test-mode [on\|off]` | Show/change the retained test session; changes pause routing; off restores everyday startup |
| `bench arm N` / `bench freeze` / `bench off` | Arm/freeze/release detailed RAM capture within a test session |

`help` and `about` print through a low-priority worker so the input task does not
wait for their UART output. Startup and unknown/unavailable-command messages
point to `help`. These commands only display information.

`about` reports automatic Git/UTC build identity and the exact `.bin` SHA-256,
alongside ESP-IDF metadata and the separate ELF fingerprint. See
[slave diagnostics and updates](slave-management.md) for remote history,
retention/clearing, firmware transfer and the one-time wired OTA migration.
For command-line firmware updates, go directly to
[Updating one or all receivers](slave-management.md#updating-one-or-all-receivers).

`history` includes current RAM records and unsaved repeats, including when flash
persistence is disabled. Event/snapshot sequence numbers identify their order
across ring wrap. Disabling persistence does not erase previously saved history.
The production S2 exposes HID only; read these commands through the S3 UART.

### Key delivery tracing

The controller continuously traces decoded keyboard presses **and releases**,
including modifiers, media keys and system keys. Normal routing records
`observed`, `routed`, `queued`, `submitted` and `confirmed` stages. Delivery
sample `id` values connect queue entries to their radio generation/sequence and
receipt, including every intermediate state in an input batch. `src=0` denotes
the merged delivery stream; nonzero sources identify local input/router sources.
USB bridge failures identify their interface binding. `dst` is the selected
dongle slot (zero means there is no selected destination).

Run `key-trace` through the UART/browser console to export recent records between
`KEYTRACE BEGIN` and `KEYTRACE END`. Successful transitions stay in a 512-record
PSRAM ring and are printed only on request. The header reports overwritten
records; this is recent diagnostic evidence, not a permanent typing transcript.
It contains HID key usages, which can reveal typed input. It resets on restart.

An input that cannot be forwarded logs an automatic warning with its HID page,
usage, down/up direction, destination and reason. For example:

```text
key missing t=12345 dst=2 src=0 id=91 gen=3 seq=0 page=07 usage=0004 down reason=delivery-buffer-full
key unconfirmed t=12346 dst=2 src=0 id=90 gen=3 seq=42 page=07 usage=0004 up reason=receiver-restarted-receipt-unknown
```

`missing` means that event was discarded before radio submission. `unconfirmed`
means a submission occurred but there is no matching dongle receipt: it may have
arrived. Reasons cover a missing route, recovery suppression, full/expired
delivery buffers, unavailable destination USB, lost radio contact, missing ACKs,
receiver restart, stopping/changing a route, and USB queue overflow, stale reports,
recovery or source invalidation. A radio that never accepts the pending input is
distinguished from a missing dongle ACK. A release lost from the USB queue names
the released key using its previous decoded state.

Shortcut trigger taps are recorded as `deferred` while awaiting a second tap or
command, then replayed through delivery or recorded as `consumed` by the local
shortcut. Configuration/Keyboard Console input, held-key blocking and two inputs
sharing one held destination key also have explicit local-consumption reasons.
These intentional decisions do not create missing-key warnings.

`confirmed` proves authenticated acceptance into the destination dongle's USB
output queue, using an exact ACK or status receipt. It does not prove completion
of the PC's USB transfer or application processing. The trace starts after HID
decoding; it cannot detect a physical press that never reaches the controller or
cannot be decoded. Allocation failure and warning-queue overflow log an explicit
incomplete-audit message; buffered warnings print on a low-priority worker.

Native regression tests cover receipts, lost/duplicate/wrong ACKs, batched key-up
events, route changes, ambiguous receiver restarts, overflow/suppression, shortcut
replay/consumption, multiple keyboards, media/system keys, USB backlog loss, trace
retention and logging failures. Physical typing and RF checks remain required.

### Open the console from PowerShell

Connect the master's **COM/UART** connector, then run from the repository root:

```powershell
pwsh -NoProfile -File .\scripts\watch-controller.ps1
```

This finds the connected CH343 bridge by USB VID/PID `1a86:55d3`, locates the
pinned ESP-IDF Python, and sends `history` immediately. Type `help` for the full
command reference, `about` for firmware information, or `status`/`history` for
another dump. Press Enter to send a command. Type `.quit` to close; otherwise the capture
ends after one hour. Output is saved when capture ends to the timestamped
`build/controller-*.log` path printed at startup. The helper does not reset,
flash or change the selected computer. Close other serial monitors first.

To get just the current COM port, without opening it or requiring the toolchain:

```powershell
pwsh -NoProfile -File .\scripts\find-controller-port.ps1
```

Discovery identifies the USB bridge, not the running firmware. If several
matching bridges are connected, it lists their ports and serial numbers and
requires a choice. Replace the example bridge serial below with the one reported for your controller.

```powershell
pwsh -NoProfile -File .\scripts\watch-controller.ps1 -SerialNumber DEMO000004
```

Use `-Port COM8` to bypass discovery for another bridge type or an explicit port.
Use `-Command help` or `-Command about` to request those screens when opening the
console instead of the default `history` dump. `.quit` is handled by the PC
capture helper; it is not a firmware command.

For a bounded unattended capture with current status every five seconds:

```powershell
pwsh -NoProfile -File .\scripts\watch-controller.ps1 -NonInteractive -Seconds 30 -Command status -StatusInterval 5 -OutputPath .\build\master-status.log
```

`powershell.exe -NoProfile -ExecutionPolicy Bypass -File` can replace
`pwsh -NoProfile -File` when using Windows PowerShell 5.1.

### Radio transmit health and clipboard contention

`status` includes a `KMSRADIO status` line with the selected route, pending
packet type/sequence/generation, buffered input count and oldest age, pending
ACK age, and time since receiver contact. Its TX counters distinguish accepted
driver submissions, completions, MAC delivery failures, immediate send errors
(`rejected` and numeric `last-error`), and unexpected callbacks. `busy`, `age`
and `max` describe the outstanding and longest completed driver transmission,
separately from the application ACK deadline. `peer` and `bytes` identify the
destination and size of the most recent submission. Paired receiver lines also
show the most recently received input/status RSSI. An offline peer's reading can
be stale; zero means no reading has been recorded since boot.

`KMSCLIPBOARD` reports accepted USB and radio packets, session/authorization
rejections, relayed/delivered packets, queue drops, and the current command,
incoming and outgoing queue depths. `deferred` counts scheduler checks that
reserved the radio for input; it is not a count of lost clipboard packets.
These diagnostics contain counters and protocol metadata, never file names,
clipboard content, keys or pointer coordinates. New input ACK timeouts capture
the same two lines automatically, at most once every five seconds, into the
existing volatile PSRAM log. Use `logs` before restarting to retain that evidence.

Clipboard packets remain queued while the master waits for an input or typing
ACK, releases or activates a route, or recovers input delivery. Receivers defer
clipboard transmission while either normal or typing receipts are pending.
Desktop control and firmware management keep their existing precedence. Busy
input can therefore slow a dongle-only file transfer; end-to-end requests retain
their existing retry limits and may still time out under sustained load.

If an S3 driver send has no completion callback for one second, the application
can restart Wi-Fi/ESP-NOW and restore the saved encrypted peers on the same
channel. It retains the route, generation and input sequence so a receiver can
deduplicate an input already applied. This does not reboot the S3 or erase
pairing. Recovery is deferred during pairing, firmware/history transfers, channel
changes, unconfirmed OTA boots and hardware test sessions. It is limited to
three attempts per ten-minute window; a failed SDK operation or exhausted budget
blocks further sends and logs that the S3 needs a manual restart. Receivers keep
their existing lease fail-safe and do not automatically restart their radios.

This completion watchdog does not trigger for intermittent ACK delays when the
driver continues completing sends. Strong RSSI does not exclude interference,
and these counters do not establish the cause of a particular timeout storm.

### Automatic receiver restarts

The S3 can request a software restart of a paired S2 after repeated delivery
faults. Automatic recovery is enabled by default. `receiver-recovery` prints its
configuration and saved per-dongle budget; `receiver-recovery off` disables it
immediately, and `receiver-recovery on` enables it after saving the setting.
`status` includes each receiver's live failure window, cooldown and restart wait.
`KMSRECOVERY` messages distinguish scheduling, evidence collection, command result,
new-boot confirmation, deferral and timeout.

| Receiver | Default trigger | Minimum span between first and last fault |
| --- | --- | --- |
| Inactive | 4 incidents within 15 seconds | 2 seconds |
| Active | 12 incidents within 30 seconds | 5 seconds |

Incidents include failed MAC completions, completions taking at least 100 ms,
and input acknowledgement timeouts. Faults within 200 ms coalesce into one
incident; ten seconds without a fault clears the window. A short isolated burst
cannot become eligible just by waiting. The receiver gets five seconds of grace
after boot or USB readiness returns.

Only recently heard receivers with USB ready, restart support and a confirmed
firmware identity are eligible. Sleeping/disconnected USB hosts and offline
dongles do not trigger automatic restarts. Firmware transfers, unconfirmed
controller firmware, pairing, channel changes and explicit test mode defer
recovery. Recent S3 send-submission errors or a blocked S3 transmitter also defer
it: these are not evidence that an individual S2 needs restarting.

Only one receiver recovers at a time, with at least 30 seconds between attempts
across the desk. Per-dongle cooldown grows from one minute to two, four, eight
and at most fifteen minutes, with at most three attempts in any rolling hour.
Ten healthy minutes clear the escalating penalty, but do not erase the hourly
budget. Attempts are saved on the S3 before sending the command and bound to the
receiver's MAC address. S2 restarts do not clear them. After an S3 restart, saved
attempts conservatively occupy a fresh full hour and impose a fifteen-minute
cooldown because no wall clock is assumed. Storage failure prevents a new
automatic command. Thresholds and timings use `KMS_RECOVERY_*` settings.

For the active dongle, the S3 releases and pauses routing before the request.
It resumes only after seeing a different, confirmed boot with USB ready, and
preserves any manual selection or pause made during recovery. A definitive
command rejection can restore the old route; an uncertain outcome waits up to
20 seconds for confirmation, then leaves routing paused for manual selection.
Queued automatic commands are discarded if recovery is disabled or their
receiver boot, readiness, deadline or operational guards change.

A receiver whose radio cannot receive the request still needs a physical reset.
Automatic restart is bounded mitigation, not a diagnosis of the underlying stall.
See [receiver evidence and compatibility](slave-management.md#radio-evidence-across-restarts).

## Configuration

All defaults are in `config/kms_settings.h`; rebuild the S3 after changes.

| Setting | Default | Meaning |
| --- | --- | --- |
| `KMS_RECOVERY_BUFFER_DEPTH` | 256 | Maximum queued input samples |
| `KMS_RECOVERY_BUFFER_MS` | 5000 | Maximum backlog age and outage buffering window |
| `KMS_RECOVERY_REPLAY_MS` | 10 | Minimum spacing while draining a backlog |
| `KMS_DIAGNOSTICS_ENABLED` | 1 | Enable diagnostic counters/history |
| `KMS_DIAGNOSTICS_PERSIST` | 1 | Save events and checkpoints to NVS |
| `KMS_DIAGNOSTICS_STATS_MS` | 3600000 | Statistics checkpoint interval |
| `KMS_DIAGNOSTICS_IDLE_MS` | 5000 | Inactivity threshold for idle statistics |
| `KMS_DIAGNOSTICS_EVENTS` | 1024 | Retained event slots |
| `KMS_DIAGNOSTICS_SNAPSHOTS` | 72 | Retained statistics slots |
| `KMS_DIAGNOSTICS_REPEAT_MS` | 60000 | Event grouping and write retry interval |

The dedicated history partition accommodates these rings, including NVS metadata
and spare space for updates/garbage collection. The write queue has 16 entries
(`KMS_DIAGNOSTICS_QUEUE_DEPTH`), independent of retention. Each input-loop tick
checks at most eight event slots (`KMS_DIAGNOSTICS_SCAN_BATCH`) for deferred writes;
unsaved records retry after one second, grouped repeats after one minute.
`status` reports history RAM size, current free heap and largest free block.

Installing this partition requires one wired partition-table/application update;
see [master updates](master-updates.md#expanded-history-layout). Application OTA
alone cannot add the partition. History starts fresh; old records are not imported.
Pairings and preferences remain in their original NVS partition. A missing or
failed history partition leaves RAM history and input forwarding running.

Master events/checkpoints print incrementally in ring order. New events can
continue arriving during the dump; entries overwritten before their turn cannot
be recovered from that dump. Receiver histories use a separate immutable snapshot.
The browser retains up to 10,000 console lines/4 MiB for export while rendering
only the most recent 700 lines.

## Validation

`scripts/test-core.py` connects the production sender state machine to the
production receiver guard using a fake radio. Cases cover ordered key/mouse
replay, lost acknowledgements, stale status, receiver restart, the previous
master's lease, recovery without user input, USB resume, fixed capacity/age,
discarded movement, explicit destination changes and clock wrap. Diagnostic
model checks cover repeat counts, idle/held input, the hourly boundary and ring
wrap. The shortcut/radio handover suite connects the real Insert shortcut router,
sender and two receiver guards to an asynchronous fake radio. It checks the first
selection after disconnecting the active dongle, with no wait or a five/ten-second
wait, from either keyboard and across clock wrap. Slow MAC completion, rejected
old-PC releases, unavailable targets and old input isolation are covered. Existing
parser/router and asynchronous USB LED checks remain enabled.

Physical interference/replug tests, long-running hourly storage and flash
failure injection still require hardware validation before calling this rollout
verified on working devices.

## Live diagnostic messages in PSRAM

Ordinary S3 ESP log output is mirrored to UART by a low-priority worker. The
input task formats and retains each message, then queues it without waiting for
serial transmission. This prevents a slow console or a burst of fault/recovery
messages from stalling ACK processing and creating further timeouts. The live
queue holds 32 messages in task memory (preferring PSRAM). If it fills, only the
UART copy is dropped; the retained log remains available through `logs`.
`logs-status` reports `live_async` and `live_dropped`. Initialization failure
falls back to the previous synchronous writer and reports `live_async=no`.

`KMSRADIO ACK` adds matched ACK count, last/maximum round-trip time and the
last/maximum receive-dispatch delay. A large dispatch delay means a packet was
already received by Wi-Fi but the application had not handled it yet.
`KMSRADIO PEER` separates completed/failed/maximum-duration sends for input,
desktop, clipboard and other traffic on each paired dongle. `KMSRADIO SLOW`
shows the four most recent slow or failed MAC completions from a bounded
16-record ring, with recipient slot, traffic kind, operation, sequence, byte
count and duration. Kind values are 0=input, 1=desktop, 2=clipboard, 3=other;
desktop operations and clipboard kinds use their published protocol numbers.
The records contain no file content or input values. These traces distinguish
radio airtime/failures from application scheduling delay; they do not by
themselves identify an electrical or driver fault.

The S3 controller captures normal ESP-IDF log messages as they happen into a
4 MiB PSRAM ring (8,192 records, up to 506 bytes per record). It retains up to
one hour, removing older messages automatically. If logging fills the ring first,
the oldest records are overwritten. Oversized messages end with `[truncated]`.
`logs-status` reports retained record count, oldest age, capacity, capacity
evictions and message truncation counts. The age is measured from controller
uptime, not a wall clock. Bootloader/early boot messages before application
capture starts and direct console protocol `printf` responses are not captured.
Capture follows the configured log levels; it does not enable debug logging.

Run `logs` in the UART or browser console to export the retained messages.
It exports a fixed snapshot on a worker while input handling and capture continue.
Live UART logs can interleave with the export. If records are overwritten or age
out during a long export, it stops and reports that condition. `history` remains
the separate retained event/checkpoint history; `stats-save` does not persist this
live message buffer.

Without the console cable, focus an empty plain-text editor on the selected PC,
double-tap the configured trigger (for example **Insert, Insert**) and press
**L**. Release the shortcut keys; the controller types the snapshot through that
PC's receiver. Press **I** while printing to enter Keyboard Console (described
below). Any other new keyboard key, modifier, media key or system key on either
input keyboard stops the dump. The stopping key is consumed until released.
Mouse movement does not stop the dump, and mouse output is suppressed while it
runs. D keeps its existing default boot dongle shortcut.

Typing uses a US keyboard layout and the receiver's known Caps Lock state;
choose a matching layout. Tabs become spaces; unsupported bytes become `?`.
Updated receivers accept batches of up to 64 characters and generate the USB
key presses/releases locally, with at least 5 ms between keyboard reports.
The controller sends any available partial batch immediately, including one final
character without a newline. A batch is
acknowledged only after its final USB key release completes; radio retries do
not type it again. Escape and console entry cancel queued receiver text and
release synthetic keys. Batches belong to the selected receiver boot and route;
changing routes discards them. Logging continues, and the export never chases
new messages. USB delivery still limits the speed of large exports.

Both controller and receiver firmware need updating for faster typing. A brief
capability probe falls back to the original acknowledged press/release path on
older receivers, retaining its 5 ms minimum between reports. Older controllers
continue to work with updated receivers. Batch typing still needs hardware
throughput and interference validation; native tests exercise final partial
batches, USB completion, retries, cancellation and the compatibility fallback.
Typing stops if the buffer is overwritten/ages out, the route changes, delivery
fails or makes no progress for three seconds. An unavailable receiver, empty
buffer or unavailable PSRAM rejects the shortcut with the error indication.
Unknown keyboard LED feedback stops typing to avoid corrupting letter case.
A working receiver dongle is required even when the console cable is absent.

The buffer is volatile: restarting, flashing or removing power loses it.
The PSRAM capture itself only needs controller firmware. The S3 N16R8 defaults
enable octal PSRAM at 40 MHz with the startup memory test and explicit PSRAM
allocation; standard `malloc` remains internal. If PSRAM is absent, firmware can
boot and normal input continues, but `logs-status` reports capture unavailable.
PSRAM initialization and the typed export still require verification on hardware.
For existing build directories, use `scripts/idf.ps1 -Project controller -ResetConfig`
once to apply the new PSRAM defaults.

### Keyboard Console in a text editor

Open Notepad or another plain text editor and focus the document. Start typing the
logs with the usual double-tap shortcut followed by **L** (for example,
**Insert, Insert, L**). While the logs are still printing, press **I** to switch to
**Keyboard Console**. The device types a welcome message explaining that you can
type `help` and press Enter for commands, or press Escape when idle to exit.

Type a command and press Enter; the device types your command and its results into
the document. This uses the same command handler as the serial console, including
`help`, `about`, `status`, and the configuration and diagnostic commands. Backspace
corrects the command being entered. Input and output use a US keyboard layout;
Caps Lock and Shift are accounted for. Hold no keys while waiting for output.
While text is printing, Escape discards the remaining output and waits for
another command. Late output is suppressed until you start typing a new command.
When no text is printing, Escape ends the session and restores normal keyboard routing. Other keys still
cancel an ordinary log dump before you enter Keyboard Console. A receiver change,
link failure, stalled output, or overwritten output also ends the session.

Console output includes asynchronous worker responses and new diagnostic logs.
Commands that pause, switch, unpair, restart, or update the active route can end
the session before their response reaches the editor. A serial console remains
available for those operations. No application or terminal connection is needed
for Keyboard Console itself.

### USB frame guard

The normal S3 master also applies a bounded USB channel-start timing guard.
Its counters appear in `usb-trace` and retained fault snapshots; see
[USB frame guard](usb-frame-guard.md) for behavior, limits and driver integration.

### History time ranges

Both the serial console and Keyboard Console accept:

- `history`: all retained history, including earlier boots, as before.
- `history 10`: events and statistics from the last 10 minutes.
- `history 20 30`: entries between 20 and 30 minutes ago, including both boundaries.

The two ages may be supplied in either order. Use whole, nonnegative minutes
(up to 71582). Ranges are measured when the command is queued, so worker delays
do not shift the window. Ranged output includes only the current boot: retained
entries from earlier boots have no wall-clock timestamps that establish their age
relative to now. Event ages use their recorded timestamp; grouped repeats retain
the original event timestamp. Current statistics appear when the range includes
zero minutes ago. A range with no matching entries still prints its header and end.
