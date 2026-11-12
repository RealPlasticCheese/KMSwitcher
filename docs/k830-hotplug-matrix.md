# K830 hot-plug allocation regression matrix

This matrix checks endpoint allocation, shared heap pressure and bounded reconnect behavior.

The fix retains only the parsed input fields. Full workspaces still accept 64
fields and 32 usages per field. LED fields, report lengths and output safety
metadata precede the input-field array and remain intact when its unused tail
is released. Compaction happens before the parser is published to callbacks.
Aligned endpoint resources are reserved before allocating the full workspace.
For the four observed interfaces (2/7/2/6 input fields), retained parser storage
falls from 31,120 to 11,044 bytes, saving 20,076 bytes.

`KMSUSBMEM` status retains first/latest USB allocation failure context, failed
request size/capabilities/function and current/default/DMA free/largest blocks.
The allocation callback only records fixed metadata. Sequence and task checks
prevent stale or unrelated task/ISR failures being attributed to a USB claim.

## Run all automated layers

```powershell
python scripts/test-usb-hotplug.py
```

This runs offline on Windows with MSVC and the repository's pinned ESP toolchain.
It does not open serial ports, flash boards or disrupt the working keyboards.
It includes the production lifecycle matrix, selected actual SDK allocator
functions, the existing HAL/LL matrix and Linux stress/report regressions.

### Production lifecycle: 1,052 cases / 25,218 assertions

The native harness compiles `usb_devices.c` directly. It uses asynchronous
completion/cancellation and compact synthetic HID descriptors with the same
keyboard/pointer/unsupported-interface layout as the captured receivers. These
are not byte-for-byte Logitech descriptor or RF implementations.

| Axis | Values |
| --- | --- |
| Replugged layout | c52b: keyboard, pointer, unsupported interface; c534: keyboard, pointer |
| Other receiver | Absent; present and continuously delivering keyboard/pointer reports |
| Interrupt packet size | 8; 64 bytes |
| Failure point | None; first interface claim; second claim; parser; descriptor/control transfer allocation; input transfer allocation |
| Pressure duration | One failure; three failures; persistent until cleared explicitly |
| Replug ordering | Old device drained then fresh address; fresh address while old cancellations are held; same address reused after destruction |
| Clock | Normal; crosses uint32 uptime wrap |

The base cross product has 768 cases. Additional groups cover:

| Group | Cases | Required result |
| --- | ---: | --- |
| Unplug, disable/re-enable, explicit rescan, controlled root reconnect during pressure | 40 | No unsafe free or latched retry; return when resources are available; ordinary retries preserve the other receiver |
| Disconnect/replug at nine descriptor/protocol/claim timing points | 216 | Old callbacks drain before new claims; no stale owner/address use |
| Both receivers repeatedly unable to allocate, either arrival order | 20 | Bounded retries, no leaks; healthy EP0 traffic alone must not be claimed as proof of usable inputs |
| SDK control channels still owned after disconnect | 8 | Standby until actual capacity returns; no premature claim; surviving receiver continues |

Every case checks the channel budget, parser/transfer ownership, eventual input
restoration after pressure clears, and full cleanup. Persistent unavailability
is deliberately characterized: retries cannot create memory that remains
unavailable. Explicit rescan retries unsupported descriptors too; under global
parser pressure that can temporarily affect both devices.

The matrix exposed and fixes a separate policy-isolation bug. A setting change
for one receiver previously reset the other receiver's cached unsupported
interface to probing. That invalidated its entire bundle and unnecessarily
retired/reclaimed working inputs. Cached unsupported interfaces now stay cached
on policy changes; explicit rescan remains available. An isolated copy of the
previous implementation must fail this regression.

### Selected SDK allocation functions: 1,479 cases / 50,766 assertions

`test-usb-allocation.py` extracts six **unmodified function bodies** from the
installed, locked USB 1.6.0 component: `endpoint_alloc/free`, `hcd_pipe_alloc/free`
and `buffer_block_alloc/free`. It records complete source and function SHA-256s,
the dependency lock and the native adapter details. No SDK file is edited.

| Axis | Values / checks |
| --- | --- |
| Transfer class | Control, isochronous, bulk, interrupt allocation sizes |
| Existing channels | 0 through 8 occupied; channel exhaustion returns `ESP_ERR_NOT_SUPPORTED`, distinct from memory failure |
| Allocation failure | Each of seven endpoint/pipe/channel/buffer-metadata/DMA allocation calls, plus success |
| DMA alignment | Valid 512-byte alignment; deliberately invalid allocation rejected and freed |
| Cache alignment adapter | 0 and 32 bytes; correct descriptor-list accounting |
| Fragmented DMA pool | 16 pinning offsets, two strides; ample total free bytes but no suitable aligned descriptor region; eight failed retries without leaks |
| Repeated replacement | 1-8 allocated pipes; every replacement position and failure point; eight cycles; sibling descriptors remain intact |
| Disconnect during allocation | Initial invalid port and six points before the second connection-state check; partial resources freed |
| Busy free | Executing buffer or owned URB prevents premature release |

Four deliberately broken SDK copies must fail: leaking a DMA list, leaking
partial pipe buffers, leaking the endpoint when pipe allocation fails, and
omitting the alignment check.

The deterministic heap model reproduces persistent aligned-allocation failure
despite more than 99% aggregate DMA bytes free. Releasing the simulated pressure
restores allocation. This demonstrates a possible mechanism; it does **not**
prove that the live S3 had that heap layout, nor identify an upstream leak.

The native port/pipe/endpoint types and heap, queue, cache and channel operations
are narrow adapters. The QTD pointer is represented as uint32 on x64 to preserve
the S3 descriptor's eight-byte allocation size. These tests do not execute the
complete HCD/USBH scheduler, ESP's TLSF allocator, FreeRTOS/ISR timing, hardware
DMA/cache behaviour, PHY, hub power or Logitech RF firmware. The separate HAL/LL
suite exercises the actual channel/register algorithms.

### Coupled production lifecycle / SDK / TLSF regression

`test-usb-shared-heap.py` additionally links the production `usb_devices.c`,
the selected SDK allocator bodies and the pinned ESP TLSF allocator together.
Parsers, SDK metadata and aligned descriptor lists compete for the same pool;
a second non-DMA fallback pool models S3 capability priorities. DMA sizes are
rounded to requested alignment as in `esp_heap_adjust_alignment_to_hw`.

The test covers 49 shared pool sizes (24-48 KiB), two receivers, and eight
receiver reconnects per size. The previous full-size parser retention policy is
compiled separately and must fail input restoration, even with endpoint-first
ordering retained; compact parsers must keep
both receivers active, restore after every reconnect and free every allocation.
A compact parser also decodes keyboard input and encodes LEDs after released
storage is reused; maximum parser capacity is unchanged.

The native TLSF adapter uses x64 headers, portable bit-scan/optional hook shims,
three byte-pointer casts for GNU pointer arithmetic, and padding before the
x64 pointer table. These compatibility edits are confined to generated build
output; installed SDK files remain untouched. This is a coupled allocation
regression, not a byte-for-byte replay of the target heap or a full scheduler,
DMA, PHY or RF emulator. Source hashes and adaptations are recorded in
`build/usb-allocation-tests/shared-heap-sources.json`; the before/after verdict
is in `build/usb-allocation-tests/shared-heap-before-after.json`.

### Captured evidence and stress reporting

Linux regressions retain unmodified relevant lines from the K830 incident. All
three SDK endpoint `ESP_ERR_NO_MEM` spellings now become a specific
`host-endpoint-allocation-failed` classification. Repeated errors are counted in
one fault entry so a retry loop cannot fill the fault window and hide a later
control fault. Snapshots stay immutable; stale attempts, receiver-side UART
messages, channel exhaustion, reset and missing evidence are checked separately.
Trace and USB-diagnostics boot IDs are independent; the collector compares each
with its own baseline across cases and still checks for a trace reset around
every USB status read. Different IDs between those systems are not a reboot.

The test scripts generate these **local reports and provenance files** beneath
the `build/` directory of the checkout or worktree where they run. These are
generated test outputs: `build/` is ignored by Git, so the JSON files are not
committed, pushed or included in the pull request. The scripts and scenario
documentation are tracked; running `python scripts/test-usb-hotplug.py` generates
the reports in that checkout:

- `build/usb-host-tests/usb-hotplug-tests.json`
- `build/usb-host-tests/policy-before-after.json`
- `build/usb-allocation-tests/matrix-report.json`
- `build/usb-allocation-tests/source-manifest.json`

## Physical follow-up matrix

The Linux `usb-stall` campaign already varies input, rate, idle/wake, LEDs,
console and radio-status traffic and will now identify allocator failures in
those captures. It uses synthetic generators and does **not** physically unplug
individual Logitech receivers or toggle the manual hub switches. Its readiness
override changes receiver-to-PC readiness, not S3 input enumeration.

For later controlled physical testing, cross these dimensions and retain the
selected case, timing and hub layout with each capture:

| Axis | Values |
| --- | --- |
| Hub | EDO 4 port USB hub; Temu 4 switched port special |
| Receiver removed | K830/c52b; other keyboard/c534 |
| Input layout | One receiver; both |
| Activity at removal | Idle; typing/pointer input; LED changes and input |
| Reconnect timing | Immediate; after cancellation/drain; after a longer idle |
| USB path | Receiver alone; whole hub; individual switched port where supported |
| Placement | Adjacent radios; separated radios; recorded hub/board spacing |

Capture bounded `status`, `usb-list`, `usb-trace` and `usb-fault` output before
recovery, then verify four expected active interfaces and both input paths after
reconnect. Avoid the full master `history` dump. The follow-up session captured
an EDO hub restart and two K830 reconnect failures on the working input path;
the remaining grid is not claimed as physically tested.
