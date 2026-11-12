# USB frame guard on the normal master

The ESP32-S3 controller uses the pinned USB Host 1.6.0 / ESP-IDF 6.1 driver with
the frame-phase guard enabled by default. It is applied in the HAL immediately
before the channel-enable register write, for interrupt-IN endpoints only.
Application HID callbacks, routing and the normal recovery policy are unchanged.

The same guard implementation is compiled by the USB-isolation diagnostic image.
Its existing `iso workaround 0|1` / runner `--workaround false|true` control is
preserved. The normal controller always boots enabled and has no new persisted
setting or console toggle. S2 receiver firmware does not use the guard.

## Behavior and bounds

For a full-speed 1 ms USB frame, the guard avoids the first and last 20 us.
It scales the frame-clock margin from HFIR rather than assuming one clock rate.
An activation already in the safe window proceeds immediately. Otherwise it
polls for a safe phase, with a 64 us timer budget and a separate 2,048-read limit.
Invalid intervals, a stopped clock or an exhausted budget increment `timeouts`
and fall through to the normal SDK activation. The guard never spins indefinitely,
resets/halts the bus or changes descriptor ownership. Existing recovery still
handles failures that the guard does not prevent.

The hook runs while the HCD holds its existing critical section. It adds no
sleep, heap allocation or UART output there. Guard hooks are placed in IRAM and
their fixed counters/lock in internal DRAM. The wait can extend interrupt/owner
latency by tens of microseconds; the four-hour diagnostic run observed a maximum
40 us wait. The software bounds are not a measurement of every hardware case.

This remains a workaround for a suspected channel-start/frame-boundary timing
interaction. A four-hour guarded hub run passed; an unguarded direct-connected
run failed with the same pending-descriptor pattern. Some unguarded crossings
succeed, so a crossing alone is not a demonstrated sufficient cause. The
12-hour guarded direct test was still running when this change was prepared.
No completed overnight result or production-device validation is claimed here.

## Diagnostics

Run `usb-trace` from the existing console. Its copied driver snapshot includes:

```text
KMSUSBTRACE WORKAROUND enabled=1 guard-us=20 budget-us=64 activations=... waited=... timeouts=... max-wait-us=... crossings=...
```

- `activations`: eligible interrupt-IN channel starts since boot.
- `waited`: starts that entered the wait path, including waits that timed out.
- `timeouts`: an invalid interval or a wait that could not reach a safe phase.
- `max-wait-us`: largest observed wait-loop timer duration.
- `crossings`: enable-write brackets whose frame number changed.

Wait counts are timing windows avoided, not proven prevented stalls. Nonzero
timeouts/crossings mean protection was incomplete and warrant examining the
retained fault and bus history. Counter arithmetic wraps at uint32, as with the
existing driver diagnostics. `usb-fault` includes the copied counters captured
before recovery; printing a queued snapshot never reads live driver state.

## Driver integration and upgrades

`firmware/usb_frame_guard/` holds the shared implementation, header, source
generator and controller CMake integration. Diagnostic compatibility shims keep
the standalone reproducer's API/source layout. The normal master does not link
the diagnostic HCD/DMA trace store or generator firmware.

The build generates a guarded copy of the exact reviewed HAL translation unit,
checks its SHA-256 and activation anchor, and replaces exactly one HAL source
in the target. The installed SDK and managed USB component are never edited.
Changing SDK source/target or USB component version requires an explicit review;
the build fails rather than applying the hook to an unknown layout. Incremental
build dependencies include the SDK source and generator scripts.

## Regression checks

```powershell
python scripts/test-usb-stack.py
python scripts/test-stress.py
python scripts/test-usb-host.py
pwsh -NoProfile -File scripts/idf.ps1 -Project controller
pwsh -NoProfile -File scripts/idf.ps1 -Project usb-isolation
```

The shared guard's native clock model covers 216,000 frame-phase, endpoint and
mode scenarios, including the captured FRREM=17 hub failure and FRREM=15 direct
failure, clock/timer stoppage, wrap, boundaries and fallback behavior. The SDK
suite compiles the actual generated production HAL and shared guard against RAM
registers: 768 cases cover all eight channels, both clock rates, all transfer
types/directions, enabled/disabled modes, safe/unsafe phases and timeout fallback.
It checks that SDK channel enable, descriptor-list encoding, descriptor contents
and scheduler state are preserved. Removing the hook or broadening its endpoint
scope must fail the matrix. Existing hot-plug/recovery/console snapshot tests
remain part of validation. These checks do not emulate physical USB DMA timing.

Normal controller builds and diagnostic builds are verified separately. The
isolation source package includes the shared guard files so the extracted
reproducer remains buildable without the rest of the repository.
