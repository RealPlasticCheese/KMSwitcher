# Routing reachability and regression checks

The production S3 router in `firmware/components/kms_core/desktop_routes.c` skips
sleeping, hibernating, shut down, stale, unready, disabled and unreachable PCs.
Select a sleeping PC manually with the keyboard shortcut before using input to
wake it. Automatic crossings always require a responding destination companion.

## Algorithm

1. Group each PC's overlapping or edge-connected monitors. Disjoint monitors
   remain separate vertices so a disconnected monitor cannot become a trap.
   Physical routes use actual exposed shared spans on both PCs, including
   multiple directions at an inside corner. A laptop's upward contact does not
   extend to the top of another monitor belonging to that PC.
2. For each connected region of unavailable monitors, connect its available
   bordering groups where unobstructed reciprocal crossings exist. Repair any
   remaining islands with distance-ordered links. Temporary links use free edge
   regions and cannot override physical contacts or jump past a nearer awake PC.
   Saved positions and remembered screen shapes stay unchanged.
3. Split exposed edges at monitor-boundary changes. Within each span, physical
   neighbours have priority; divide only competing eligible destinations by
   position along the edge. A temporary link is eligible only if no nearer awake
   PC blocks that direction. Preserve perpendicular position for aligned physical
   crossings; map temporary crossings to a reciprocal band two pixels inside a
   real monitor. Internal seams and excluded corners never switch.

With PC 2 asleep in the reported layout, PC 3's left edge can reach PC 1.
Adding PC 4 left of the laptop and PC 5 above the large screen gives PC 3 direct
routes to those bordering PCs where another awake PC does not block the crossing. Position along the edge selects the target;
slot numbers do not determine direction. A gap or corner-only arrangement can
require a virtual link. Insisting on straight aligned crossings would strand
some awake subsets, for example when only PCs 3 and 5 remain available.

Reachability means a sequence of mouse crossings can reach every available
monitor group, assuming a stable layout, working helpers/transports and enough
usable edge pixels for the assigned neighbors. With at least two available PCs,
the planner joins available groups using unobstructed crossing regions.
If no usable reciprocal connection exists, it refuses automatic routing safely. Each required link has a usable band in both directions.
With fewer than two available PCs there are no automatic cross-PC routes.
An edge-capacity failure safely refuses automatic switching; manual selection
remains available. Tiny monitors with large corner exclusions can cause this.

## Interaction and implementation limits

Physical contacts retain their exact shared monitor span and take precedence over
all temporary routes. Temporary bands divide only unobstructed spans between
eligible neighbours. The current editor shows saved screen placement, not those
temporary bands. Exact overlays remain deterministic but visually ambiguous.
Tests do not measure discoverability or comfortable minimum band sizes. A layout
with insufficient usable pixels or excessive fragmentation fails safely and
retains manual switching.

The S3 allocates a fixed workspace below 32 KiB once, preferring PSRAM. The S2
allocates no route workspace. Rebuilds happen on topology or geometry changes;
steady crossings reuse the cached graph. The spanning calculation uses bounded
arrays rather than weighted records for every monitor pair or per-pixel tables.
An allocation failure disables automatic routes and leaves manual selection.

The existing station transform maps local pixels into saved desk areas, including
negative origins and independently scaled stations. Landing is converted back to
actual target monitor pixels. No protocol version change is needed.

Queued handoffs carry topology, source generation and helper-session guards.
A power, readiness, lease, link, session, geometry or configuration change makes
an old reply unusable, including a sleep/wake cycle that restores the same final
geometry. Missing or declined acknowledgements never select a PC; an awake
alternative may be prepared only on an unobstructed crossing. A fresh awake PC
remains a barrier even when its arrival times out or is declined, so failing to
acknowledge does not let the mouse skip past that PC.

## Reproduce the checks

From the repository root, with Python and the native C compiler installed:

```text
python scripts/audit-routing.py
python scripts/audit-routing.py --seed 20261103 --output build/routing-audit-alt.json
python scripts/ci-tests.py native
```

The audit compiles the production C routing and handoff code into the existing
native test library. The same unit fixtures and reachability oracle run against
both the Python specification and compiled C. Every generated production band
is exercised at its ends and midpoint through CROSS/PREPARE/READY/SELECT. The
oracle checks real helper edge detection, non-overlapping bands, valid landings,
no unavailable destination and directed reachability of all monitor components.
The negative oracle also sweeps assigned and unassigned edge spans at monitor
and band boundaries, requiring no switch on unassigned spans, exact physical
contacts, and no jump past a nearer awake PC. A negative control deliberately
expands the laptop contact onto the large screen's top and must fail.
Production bands must also match the independently computed reference bands.
Any violation fails the command; `--require-production` is retained as a
compatibility flag and no longer changes enforcement.

Coverage includes one through seven PCs, up to sixteen monitors per PC, every
on/off subset of each layout, and every one-PC transition between those tested
states. Arrangements include a synthetic inside-corner layout and its added neighbors, rows,
columns, diagonals, hubs, gaps, overlays, negative coordinates, inside corners,
monitor rings, overlaps and disconnected local monitors. The largest case has
112 independently accessible monitor components.

Native pending-handoff tests mutate source, destination and unrelated PCs after
CROSS without resetting the firmware state, then deliver the old READY. They
cover sleep, shutdown, unready/disabled helpers, radio loss, lease expiry,
restarted sessions, screen/configuration changes and a complete sleep/wake cycle.
The existing codec, session, timeout, input-generation, manual selection and
pointer-parking receipt regressions also remain in the native suite. Independent
negative controls remove routes, overlap bands, select a sleeping target and
corrupt landing coordinates; each must fail the oracle.

Tests and local builds do not establish physical sleep/wake delivery, radio
reliability or hardware latency. Real multi-PC testing is still needed for those.

The full audit uses 400 generated layouts per PC count and every power subset.
Its JSON separately records positive-band crossings, all edge samples, unassigned
edge samples and active-PC barrier checks. The unit suite has 45 tests, including
negative controls, and the native suite covers timeout and declined-arrival
barriers. The 32 KiB workspace budget is enforced by a static assertion and tests;
100 steady crossings must reuse the cached graph.

| Seed | Availability states | Edge samples | Must-not-switch samples | Awake-barrier checks |
| --- | ---: | ---: | ---: | ---: |
| 72519 | 103,196 | 9,555,432 | 6,162,068 | 198,212 |
| 20261103 | 103,196 | 9,660,300 | 6,231,851 | 200,026 |

Both runs passed with zero unreachable available groups, ineligible selections,
active-PC skips or invalid physical spans. The complete native CI suite and all
195 companion tests pass. The corrected S2/S3 release builds also pass linked
memory budgets, including zero static bytes in the routing module.
