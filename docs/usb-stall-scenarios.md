# USB stall regression matrix

The [K830 hot-plug allocation matrix](k830-hotplug-matrix.md) adds production
lifecycle, selected SDK allocator and captured-incident regressions for the
October 21 endpoint-allocation failure. Run all of those layers with
`python scripts/test-usb-hotplug.py`.

The captured stall had advancing frames, active/unprocessed control descriptors,
no new completion interrupts or channel errors, and receive FIFO nonempty
asserted. An overdue control probe followed the last completion by 848 ms.
Tests must distinguish a recreated symptom from reproduction of its cause.

## Automated layers

`python scripts/test-usb-host.py` compiles the production KMSwitcher host,
bridge, supervisor, fault capture and tracing code with asynchronous host I/O
and deterministic clocks. `python scripts/test-usb-stack.py` compiles the actual
installed ESP-IDF 6.1 S3 `usb_dwc_hal.c`, S3 LL header and SoC register definitions.
The latter uses RAM-backed registers and writes a SHA-256 source manifest plus
the USB component dependency lock into `build/usb-stack-tests/`.

The native HAL harness replaces platform assertions, attributes, compiler
builtins and GCC register-access macros. A generated copy of the HAL header
makes its two-bit endpoint-type field explicitly unsigned: GCC's enum is
unsigned here, while MSVC would interpret interrupt type 3 as -1. This is a
host-only representation adapter. HAL/LL algorithms and installed SDK files
are not changed. Assertions are counted; unexpected aborts/hangs fail through
a bounded child-process timeout.

| Layer and scenario | Required result | Automated coverage |
| --- | --- | --- |
| HAL: normal descriptor completion versus requested halt | Exactly the correct terminal event; channel inactive | All transfer types and eight channels |
| HAL: short interrupt IN packet before halt interrupt | First event requests halt and returns NONE; later halt completes | Real HAL/LL two-interrupt path |
| HAL: STALL, babble, buffer unavailable or transaction error with completion/halt bits | Error takes precedence; error identity retained | Error/explicit-halt combinations on every channel |
| HAL: several channels interrupt simultaneously | Every pending channel is returned exactly once | Every nonempty subset of eight channels |
| HAL: disconnect/overcurrent coincides with channel IRQs | Root event takes precedence | Both overcurrent states and disconnect combinations |
| HAL: full budget, FS hub/low-speed flags, polling intervals/offsets, channel recycling | Bounded allocation and correct periodic schedule; no stale schedule bits | Three control plus five periodic channels; nine intervals; four offsets; repeated reuse |
| HAL: RXFLVL asserted, frames advance, no channel IRQ | No invented completion; active transfer remains pending | Replays the observed register signature, not DMA's cause |
| Owner: four 1 kHz input endpoints plus controls/hub | Bounded copied reports; prompt rearming; no callback allocation, logging or decoding | 2,000 simulated reports at eight-channel capacity |
| Owner: slow decode, DMA buffer overwritten after rearm | Copies preserve data/order/capture time | Callback-context and overwrite guards |
| Owner: raw queue full or reports too old | Continue polling; discard unsafe backlog and require neutral state | Saturation, expiry boundary and clock wrap |
| Owner: endpoint error, cancellation or no-device completion | Retire safely; reject its old queued copies; other receiver progresses | Every one of four endpoints, three status classes and clock wrap |
| Owner: unplug/replug, pointer/address reuse, obsolete activation | No stale input or premature release/free | Asynchronous lifecycle, generation and cancellation tests |
| Owner: parser/transfer/channel allocation failure at capacity | Retry after resources drain; no permanently latched failure | Existing hot-plug allocation regressions |
| Supervisor: two silent control pipes | Detect timeout once, freeze evidence, retain route, wait for cancellation before reconnect | Every control operation, both receiver addresses, clock wrap |
| Supervisor: one/both controls complete at timeout boundary | Remaining overdue pipe triggers recovery; both complete is healthy | Exact deadline and completion ordering |
| Supervisor: idle devices, absent hub, blocked USB calls, stalled cleanup | Idle is allowed; recover/restart within bounded budgets; no reset loop | Existing idle, absence, call-watchdog and restart-limit tests |
| Evidence: register sampling, full UART queue, changing DMA descriptors | Read-only capture; immutable fault data; bounded history | Existing pre-clear IRQ, snapshot and console ownership tests |

The HAL runner verifies test sensitivity using isolated broken source copies:
removing the short-packet halt, dropping simultaneous channel interrupts and
giving completion priority over errors must each fail. It never edits the SDK.

## Cases that need physical hardware or a richer stack emulator

The native tests do **not** execute USB 1.6.0's complete HCD/USBH/hub scheduler,
FreeRTOS/ISR interleavings, real scatter/gather DMA, cache stalls, register
write-one-to-clear side effects or physical FIFO/PHY/power behaviour. The
descriptor pointers use native pointer width; this is not a DMA ABI test.
Passing is therefore not proof that the intermittent S3 stall is eliminated.

For later controlled reproduction, retain these axes and success criteria:

| Physical scenario | Evidence to compare |
| --- | --- |
| One versus two receivers; direct versus hub; hub models; adjacent versus separated radios | Topology, endpoint count, last completion, per-channel errors and timeout order |
| Exact-MPS versus short interrupt reports, multiple bIntervals, simultaneous EP0 LED/probe traffic | QTD ownership, short-packet halt IRQ sequence, RXFLVL and channel masks |
| Sustained input, long idle then input, UART traffic, NVS writes, radio load | Callback/dispatch/poll-gap maxima and their timestamps before loss of progress |
| Hot unplug at setup/data/status, replug during cancellation, manual recovery | Bounded reference drain, reuse identity, all expected interfaces restored |
| Hub power interruption or USB clock/port change | Root/port events and frame progression distinguish electrical loss from live-frame DMA stall |

Do not run disruptive cases on the user's working input path merely to increase
test coverage. Normal-use captures remain useful while offline tests develop.
Compare future incidents with `KMSUSBSTREAM`, frozen `usb-fault` and the exact
source manifest. The existing callback mitigation is retained; no experimental
upstream driver patch is introduced by this matrix.

## Linux campaign regressions

The `usb-stall` preset described in [stress testing](stress-testing.md)
executes synthetic input on physical hardware and saves replay recipes. Its
offline tests run through `python scripts/test-stress.py`:

| Discovery from normal use | Regression requirement |
| --- | --- |
| October 20: live frames, RXFLVL, active QTDs, no new channel IRQs | Parse retained incident lines and recognize that symptom; remove each required observation and reject that signature |
| Overnight Windows updates: receiver output stalls while the PC reboots | Receiver/link events remain separate from the S3 host signature; no unproven Windows cause inferred |
| Device re-enumeration and quiet periods without S3 recovery | No host-stall claim solely from those observations |
| Earlier fault snapshots/manual captures survive into later checks | Check boot, uptime and sequence; reject stale/pre-attempt/manual evidence |
| USB recovery and time/counter wrap | Preserve uint32 wrap semantics; missing frozen evidence is inconclusive |
| Delayed UART output, full queues, detached capture | Match request ID and complete frames; bounded buffers; never report missing evidence as a pass |
| Input can fail while UART still responds | Stop on completion-progress failure, freeze traces before stopping sources, retain partial attempt |
| One source versus both and different seeds | Run the real runner/oracle with each source alone and both; an inactive source producing traffic fails |
| LED feedback/console/radio traffic | Check LED feedback and restore neutral LED state; renew heartbeat after bounded console reads |
| Recovery needed before replay | Clear test overrides, verify neutral completion and exact USB inventory; cancellation propagates; no automatic board reset/re-arm |

The campaign does not replace the shortcut/shutdown regressions or the actual
HAL/LL matrix above. Replay repeats the configured workload and records whether
the same observed signature recurs; it cannot replay the original ISR timing or
prove an electrical/SDK cause.
