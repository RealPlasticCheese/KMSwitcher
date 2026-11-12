# Switching while a computer shuts down

A delivery outage must not cancel a locally entered switching command. The
controller still blocks held PC output after a recovery buffer expires, but
keeps the current shortcut stage and its original deadline. If that interrupted
prefix expires or is cancelled, its deferred ordinary key taps are discarded.
A successful selection clears any output-block request belonging to the old route.

Physical input loss, stale USB reports, queue overflow, explicit pause, pairing
and route changes retain their existing shortcut cancellation and neutral-input
protection. A responding paired dongle remains selectable regardless of its USB state.
Unreachable or unpaired dongles remain unavailable. The production selection
gate is tested against all 256 status-flag values, with real delivery transitions,
and rejects stale/unpaired peers and invalid slots.

## Deterministic coverage

Run `python scripts/test-core.py` using the pinned toolchain Python. The real
router, delivery state machine and receiver guards are used without hardware.
`python scripts/test-switching-regressions.py` additionally runs isolated copies
of the two historical implementations and a broken replay-protection variant;
each must fail its corresponding regression while current production code passes.

| Scenario | Required result |
| --- | --- |
| Expiry before the prefix, during either trigger press/release, or before the digit | One valid command selects the destination |
| Radio silence or USB suspend while the old dongle remains reachable | Switching remains local; no old input reaches the destination |
| Buffer overflow, receive overflow, suppressed-route recovery and receiver restart at each stage | Shortcut progress survives output suppression and recovery |
| Every trigger, including both modifier sides; number row/keypad; all seven slots; two report sources; clock wrap | Equivalent selection and held-key protection |
| Digit overlaps or replaces the second trigger; either release order | Command keys remain consumed |
| Escape, unavailable destination, incomplete prefix, double-tap/selection timeout boundaries | No accidental switch or replay of discarded prefix taps |
| Held key, modifier, button, consumer or system control | Suppressed until release; fresh presses work |
| Physical source removal or full input reset | Partial command is cancelled |
| Pairing, trigger configuration and boot-destination configuration | Local modes survive delivery-only suppression |
| Late old-route ACK, lost destination ACK, busy radio, rejected old release, repeated or same-PC selection | Old receipts cannot affect the new route; activation/recovery completes |
| Destination suspends after selection and resumes later | Automatic recovery uses that destination without repeating the command |
| Old PC cannot accept its release | Its lease clears held output; input cannot migrate to the new PC |
| A pending output block followed by selection/stop in the same application batch | No obsolete block suppresses fresh input on the new route |

The initial expiry regression fails on merged master `ccad2f2` before the fix.
A second regression independently detects an obsolete block surviving a route
change. The USB-owner suite (`scripts/test-usb-host.py`) also verifies that actual
USB discontinuities still invalidate queued input and require neutral state.

This matrix covers the defined state transitions and timing boundaries. It is
not an exhaustive proof of every hardware/radio interleaving, and it cannot
reconstruct the exact keys from the reported incident retrospectively.

## Sleeping computers

A mounted but suspended receiver remains available for routing. Fresh keyboard,
mouse, media or system input requests standard USB remote wake; the PC must allow
USB wake. Selection alone, neutral releases and the receiver jiggler do not wake
the computer. Suspended input is discarded rather than replayed after resume;
if the wake key is consumed by sleep, press again after the PC wakes.
A responding receiver with no mounted USB host can also be selected and recovers
on the same destination when the host becomes available.

Requires [updated S2 receiver firmware](slave-management.md#updating-one-or-all-receivers)
for USB wake. The S3 selection change also
allows selecting older responding receivers, but cannot add wake support to them.
Hardware verification remains required: sleep while selected and while on another
slot, select the sleeping slot, wake with key/mouse, disable host wake permission,
and verify no wake from selection, release or jiggler activity.
