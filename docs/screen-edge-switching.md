# Screen-edge switching and desktop companion

## Agreed behaviour

The [routing algorithm and regression audit](routing-reachability-audit.md)
verify reachability across changing PC availability, including disconnected
monitor groups and seven-PC arrangements.

The desktop companion runs on Windows and Linux and communicates through the
S2 plugged into its current computer or dock. Dongles identify physical desk
stations, not computers. Moving a laptop between docks changes its station
automatically. Dongle 1 is the Linux NUC, dongle 2 is the middle dock and dongle 3
is the right dock. The NUC's screen is above the laptop; slot numbers do not
imply a horizontal arrangement.

The S3 stores station placement, each station's last reported monitor rectangles,
and global preferences in nonvolatile storage. Turning a computer off, restarting
a helper or rebooting/updating the master preserves the full screen shape,
including empty space above an offset laptop panel. Offline screens remain visible
in grey; remembered geometry does not make an offline computer ready for a handoff.
Each attached helper supplies its current monitor rectangles and desktop readiness.
Screen count is dynamic: closing a lid, changing docks or enabling an external
screen updates geometry while the desktop is ready.
The v1 protocol bounds each station to 16 active monitors and seven stations;
exceeding those limits reports an error and excludes automatic switching.

Crossings work left, right, up and down, only on exposed edges of the local
desktop's monitor union. Internal seams remain normal OS movement. Partially
exposed edges of offset monitors are supported. Sleeping, hibernating, shut down,
disabled, unready and unreachable PCs are skipped. Their saved screen positions
remain unchanged. Fresh ready companions with Unknown power are eligible; a
stale Awake report alone is insufficient.

Physical crossings follow only the actual shared screen spans. For example,
dongle 1 above dongle 2's laptop is reachable by moving up across their shared
edge. That contact does not enable upward switching from dongle 2's large screen
or from the unconnected parts of the laptop's top edge.

**Gap tolerance (px)** and **Overlap tolerance (px)** control how far apart
or overlapped facing screen edges can be when the controller infers a crossing.
Both default to 8 layout pixels. A value of zero disables inference for that
kind of separation. Exact contacts take priority and intervening screens block
a crossing. The controller leaves screen positions as saved.

Saving a dragged layout also saves the screen geometry used to establish its
joins. If a display change opens a larger gap or overlap, a remembered crossing
can keep its direction where the matched edges still face each other. Dragging
a screen away and back before saving refreshes that geometry. Saving only a
tolerance change keeps the existing remembered joins.

The controller bypasses unavailable screens to awake bordering PCs where the
crossing is unobstructed, and adds temporary connections if monitor groups remain
isolated. Physical contacts take priority. Temporary routes use only free spans
and never jump past a nearer awake PC. If an awake neighbour times out or declines
arrival, that gesture stops rather than skipping across its screen.

With dongle 2 asleep, moving left from dongle 3 can reach dongle 1 even below
dongle 1's physical height. Adding PCs left of the laptop or above the large
screen gives those PCs their own unobstructed crossing regions. An awake PC
in the way remains the destination. The editor shows saved placement and the
active crossing bands reported by the controller. Its key distinguishes exact,
within-tolerance, remembered and fallback crossings. Gap and overlap labels
show the measured separation, and amber hatching marks overlapping screens.

Automatic switching never wakes an unavailable PC. Use the keyboard shortcut to
select that PC manually, then use keyboard input to wake it. The dongle must
remain powered and reachable, and the PC must permit USB input to wake it.
When the selected automatic target physically aligns, landing preserves the
perpendicular coordinate. Otherwise it maps the source band to a reciprocal band
on a real destination monitor, inset by two pixels. Coordinates use the desktop
backend's pixel space and the saved station-area transform.

Each station also has a saved display-area width/height. The initial UI save
captures these from the reported desktop, and subsequent laptop/resolution
changes map into that fixed area. Neighbouring stations therefore stay aligned
when the computer or monitor count changes. Width/height can be edited to match
physical alignment through imported configuration or the CLI; zero uses reported
dimensions until the initial save. The CLI captures the current size when
placing a station, or accepts explicit `--width`
and `--height` for an offline station. These are layout units, not required OS
screen resolutions.

Local enable/disable follows the laptop. Layout and global pause follow the S3.
Closing a settings window leaves the helper running. Quitting the companion
withdraws readiness. Manual keyboard switching is retained. Explicit
[clipboard sharing](clipboard-sharing.md) is available through the tray menu.
Cross-computer dragging and app-free absolute-HID operation are future work.

## Companion settings

The tray's **Settings** opens one window with **General**, **Desk layout**,
**Clipboard**, **Transfers**, **Devices**, and **Radio** tabs. General contains
login startup, recovery notifications and pointer parking. Desk layout contains
screen placement and edge switching. Clipboard contains transport and per-type
receiving preferences; Transfers contains individual clipboard actions. Devices
contains the master's keyboard, LED and jiggler preferences. Radio contains channel
selection and resume controls. Device maintenance remains in the separate Firmware
Setup application; the companion no longer has a Set up / update devices message.

## Pointer parking

In **Settings → General**, **Park pointer after an edge switch** applies only to the
current computer and is saved automatically. Choose **Bottom right** (the default),
**Bottom left**, **Top right**, **Top left**, **Top centre**, **Bottom centre**,
**Left centre**, **Right centre**, or **Disabled**. It works on Windows and Linux X11; the existing
Wayland limitation still applies. Update the companion and S3 controller firmware
together for edge-only parking. Older controllers do not provide the required
handoff receipt, so the updated companion leaves the pointer in place with them;
existing switching remains available. No S2 firmware update is needed for parking.

After the mouse crosses an edge onto another computer, the companion parks the
source pointer once. It requires a controller receipt matching the companion's
accepted edge-crossing request and the resulting route. Keyboard shortcuts, CLI
selection and other manual switches leave the pointer where it is, including
when they interrupt a pending edge handoff. Failed or cancelled handoffs, a lost
radio link alone, startup and reconnection do not park the pointer.

With multiple monitors, parking uses the outermost edge in the chosen direction.
Corners use the leftmost or rightmost point on the topmost/bottommost edge. Centre
positions use the midpoint of that edge's span; if it falls in a gap, the nearest
real screen point is used (ties prefer the left or top point). The position is
inside a real monitor, including with stacked, offset or negative-coordinate layouts.
Monitor geometry is read again at departure.

Parking does not hide or lock the pointer. A directly connected mouse can move it
away immediately; the companion does not keep moving it back. An edge-switch
return uses the normal entry position. Bottom right usually conceals most of the
arrow, but a small tip can remain visible; Bottom left can leave more visible.
Desktop hot-corner actions can still apply; select another corner or Disabled if
they interfere.

The CLI equivalent is `kmswitcher pointer-parking MODE`, where MODE is
`bottom-right`, `bottom-left`, `top-right`, `top-left`, `top-centre`, `bottom-centre`,
`left-centre`, `right-centre`, or `disabled`. `kmswitcher --json status` includes the saved `pointer_parking` mode
and any `pointer_parking_error` from a failed placement or confirmation.

## Installation and commands

Update the S3 and all S2s used for this feature using the existing update tools.
The USB descriptor change needs an initial receiver re-enumeration; automatic
handoffs thereafter do not disconnect USB. Old receiver firmware remains usable
for manual input but does not expose this application's vendor HID collection.

Windows requires Python 3.10 or later. Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File companion/install-windows.ps1
& "$env:LOCALAPPDATA\KMSwitcher\venv\Scripts\kmswitcher.exe" ui
```

On Linux, install Python's venv support, X11/XRandR tools and Qt runtime
dependencies (Mint/Ubuntu: `python3-venv x11-xserver-utils libxcb-cursor0`). Then:

```sh
sh companion/install-linux.sh
sudo install -m 644 companion/70-kmswitcher.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
# Reconnect the receiver so the active desktop user's uaccess rule applies.
~/.local/bin/kmswitcher ui
```

The tray provides local automatic-switching enable/disable, settings, existing
device preferences and Quit. A desktop without a tray keeps the settings window
available/minimized. A headless helper can be started with `kmswitcher run`.
Login startup uses `ui --tray`, avoiding a settings window on every login.
Specify `run --serial RECEIVER_SERIAL` if more than one eligible S2 is attached;
the helper never arbitrarily chooses between dongles. The CLI/UI share one HID
owner over authenticated local IPC. Configuration works without a graphical
desktop, while automatic edge switching requires a ready graphical session.
HID permissions are still required: `uaccess` grants the active local user
access; a headless installation can use an administrator-managed device ACL or
dedicated group instead.

The Linux companion uses hidapi's `hidraw` backend for discovery and feature
reports. It reads the desktop control collection through `/dev/hidraw*` using
the installed USB rules, without claiming the USB interface from Linux's input
drivers. `kmswitcher devices` lists the compatible local receivers; if it is
empty despite a working keyboard/mouse, check the app version and receiver
firmware as well as USB permissions.

```text
kmswitcher status
kmswitcher --json peers
kmswitcher auto disable
kmswitcher auto enable
kmswitcher select 2
kmswitcher config show
kmswitcher config station 1 --x 0 --y -768
kmswitcher config station 2 --x 0 --y 0
kmswitcher config station 3 --x 3840 --y 0
kmswitcher config export desk.json
kmswitcher config import desk.json
kmswitcher config auto disable
kmswitcher preferences
kmswitcher preferences --set double_ms=350 --set keyboard_leds=true
kmswitcher startup enable
kmswitcher startup disable
kmswitcher stop
```

Those example coordinates are illustrative. Configure real placement using the
reported screens, especially when the NUC is above a particular laptop panel.
The UI shows dongle selection, layout inclusion, screen count and availability.
Drag any screen to move its dongle's group; relative screen positions follow that
computer's desktop arrangement. Coordinates and enclosing-area dimensions are no
longer table fields, so a multi-screen station is presented as its actual shape.
The UI supports dragging station groups, global pause, edge delay, corner
exclusion, and JSON export/import. Saving uses the revision
loaded by the editor. Stale imports are rejected; the CLI has an explicit
`--expected-revision` override for deliberately applying a backup to a newer
revision. Config upload is validated and persisted atomically before activation.

New dongles start in free space and the preview scales to fit the whole desk.
Drag a station near another station to snap their visible screen edges together;
nearby tops, bottoms and centres align too. Unused space inside a multi-screen
station's enclosing rectangle can contain another station. A screen can snap
into an inside corner, touching the top of one screen and the side of another.
Clicking or dragging selects the visible screen's dongle, including screens
inside another dongle's unused area.
Green lines show the selected dongle's shared screen edges; only the overlapping
part of each edge connects the screens. Snapping does not introduce screen
overlap. Exact coordinates remain available through the CLI or imported layouts.
Use **Save to master** to apply layout changes to automatic switching; unsaved
preview changes do not change the active layout.
All screens belonging to a dongle move together. **Fit layout** restores the
overview after moving stations or resizing the window. Selecting a table row
brings that station forward, including layouts imported with overlapping areas.
Automatic width/height uses the reported screens. An offline station uses its
remembered screen rectangles. Before a station has reported any screens, the
preview uses its configured area, or 800 × 450 when no dimensions are available.

You can arrange and export an offline draft without a connected receiver.
Exports show the saved path and report write failures in a dialog. To apply a
draft, export it, connect a receiver, Reload the master configuration, then
Import the draft and Save to master. An offline draft has a null revision and
adopts the loaded master's revision when imported. Backups exported from a
master keep their original revision and stale saves remain rejected.
Reload and Import ask before discarding unsaved edits; edits made while a
save or reload is running are retained.

Commands return 0 on success, 1 on operational/validation failure and argparse's
2 for command-line syntax errors. `--json` emits JSON results and JSON errors
to stderr. `desktop` is a read-only backend/monitor/pointer diagnostic.

## Desktop support

- Windows: native monitor/cursor APIs with per-monitor DPI awareness. The helper
  rejects arrival on a non-default input desktop, including secure desktops.
- Linux X11: libX11 for cursor access, XRandR for monitor rectangles, and logind
  session active/locked hints plus desktop screensaver state for readiness. Mint 22.1 Cinnamon/X11 is the
  reference machine. The helper runs as the ordinary desktop user.
- Linux Wayland: detected explicitly. The v1 helper reports automatic switching
  unavailable and retains CLI configuration. It does not fall back to XWayland
  pointer coordinates, which cannot establish the full Wayland desktop boundary.
  A future backend needs compositor-supported InputCapture and pointer-placement
  portals. Universal Wayland support is not claimed by this implementation.

No custom kernel driver is installed. App-free operation is reserved for a
future absolute-pointer backend. Geometry, station placement and routing are
independent of the desktop backend so adding it will not require replacing the
saved layout or assigning computers permanent slot numbers.

## Protocol and failure handling

The separate vendor HID collection is usage page `ff00`, usage `0002` on the
existing controls interface. Feature report 32 carries commands/replies; report
33 carries local status and pending arrival events. Both have 64-byte payloads
plus the HID report-ID byte. The payload has `K`, version 1, operation, result,
32-bit request/session IDs, 48 data bytes and a CRC32 over its first 60 bytes.
Commands are relayed in management operation 11 (`KMS_M_DESKTOP`) over encrypted
paired ESP-NOW unicast. The existing input wire protocol stays version 2.

USB callbacks copy bounded messages only. The application task processes them;
radio management is paced behind input/acknowledgement traffic. The helper polls
status locally over USB; it sends geometry only on change and heartbeats at
roughly 700ms intervals. A helper lease lasts three seconds. Each new helper
session clears live geometry/readiness even when the dock keeps the S2 powered,
but retains the remembered screen rectangles for the layout editor. USB detach
clears the local mailbox/session too. Unready desktops do not publish replacement
screen shapes. A complete, validated screen report from an attached, ready desktop
updates the master's remembered rectangles atomically before activation; identical
reports after reconnect do not rewrite flash or change the placement revision.
Existing masters learn each station's full shape the next time that computer's
companion connects after this update. Ordinary firmware updates preserve these
records along with pairing and placement; erasing NVS removes them.

After an S3 restart or firmware update, a failed heartbeat to a previously
discovered master makes the companion reopen its receiver with a fresh helper
session. This clears the S2's cached master boot ID, rediscovers the S3 and
republishes monitor geometry before announcing readiness. Local dongle logs
remain available while discovery waits for an offline master. This recovery
requires an updated companion; existing S2 and S3 firmware is supported. With an
older companion stuck on **Master unavailable**, quit and reopen the companion
on each computer after the S3 restarts.

The source must match the active routing generation and a live helper session.
The S3 validates the exposed source edge against committed geometry and its
cached route plan, excluding unavailable stations. A reachable dongle with no
mounted USB host is excluded from automatic destinations while remaining
manually selectable. The target must have a fresh, ready, enabled companion and
no explicit sleeping, hibernating or shutdown power state.

The S3 repeatedly prepares the destination for up to 300ms. The target companion
verifies its geometry version, positions the pointer and acknowledges before
selection. Missing or declined acknowledgements can cause another awake target
to be prepared; they never cause an unconfirmed switch or an automatic wake.
Power, readiness, session, topology, configuration and geometry changes invalidate
queued handoffs. A sleep/wake cycle cannot make an old READY valid again.
Keys/buttons, reversed source motion, manual route changes, geometry changes,
pairing and firmware/radio management cancel or block automatic handoffs.

Edge pressure defaults to 100ms with an eight-pixel corner exclusion. Fresh
routed mouse motion is required; S2-local jiggles and inactive local trackpads
cannot trigger crossings. A short arrival cooldown prevents bouncing back.

**Edge pressure** is the time spent pushing against an exposed screen edge
before starting a handoff. Set it to **0 ms** for no added wait. **Corner
exclusion** disables crossings within the specified distance of a monitor's
corners; **0 px** allows crossings there too. These settings affect when a
handoff starts, not how quickly mouse reports resume on the destination.

The helper waits 5ms between status polls and confirms pointer placement before
running background clipboard discovery. USB commands and route selection wake
the firmware owner immediately. For up to 80ms after selection, release,
activation and queued input take priority over management replies, including the
arrival confirmation reply. The priority ends as soon as those input queues are
clear; the time bound keeps management available if a destination fails. The
old route still receives an acknowledged release, and the new route still needs
an acknowledged neutral activation before input is delivered.

## Local dongle logs and recovery notifications

Choose **Local dongle logs…** on the companion's **General** tab. **Refresh** reads
a frozen snapshot of the attached S2's retained event journal directly over USB.
This includes earlier boots still present in its flash journal. It works without
the S3 radio connection or a network. The viewer shows boot IDs, first/last uptime,
repeat counts, event details and overwritten-record counts. Filter by boot/event
text or hexadecimal detail and use **Export…** to save a complete JSON capture.
Times are uptime within each boot, not wall-clock timestamps.

Reads run incrementally through the existing HID owner; the USB callback never
formats logs or writes flash. New events cannot change a frozen capture. A reset,
disconnect, stale token or incomplete read is reported, and export stays disabled
until a complete refresh succeeds. The local snapshot expires after 20 seconds
without reads. Radio history downloads and local snapshots are mutually exclusive
to bound receiver memory; finish one before starting the other. The viewer reads
structured diagnostic events, not arbitrary UART text or keyboard contents.

**Show automatic recovery notifications** controls recovery tray popups on this
computer. The preference survives helper/app restarts and is independent of
automatic switching and clipboard notifications. Ordinary retries stay quiet.
A newly observed confirmed automatic reboot gives a **Ready again** notice;
failed recovery gives a manual-reset warning. Informational popups are limited
to one per dongle per ten minutes, warnings to one per minute. Repeats remain
visible in the log dialog's recent recovery outcomes even when muted.

The companion queries one receiver's recovery outcome per second, rotating
through the seven slots. It does not fetch S2 logs over radio before reboot.
Startup observations establish a baseline, so old recoveries do not produce a
burst of popups. Outcomes older than a minute are consumed quietly. Delivery
requires a working radio connection to the S3; operating-system notification
settings can also suppress the actual popup.

Local logs require updated companion and S2 firmware; recovery notices also
require updated S3 firmware. Older devices keep their existing switching path
and report local logs as unavailable. The new desktop operations are local
handshake=18, journal begin/read/end=19/20/21 and recovery status=22; desktop
status byte 45 advertises local diagnostics. Journal tokens bind the host session
and receiver boot, and each record uses the existing CRC-protected HID envelope.

## Companion crash diagnostics

The packaged tray launcher keeps a separate process outside Qt. If the tray
process exits with an error, it records the exit code and restarts the tray,
with delays of one, two and four seconds. A fourth failure within a minute
stops recovery. Intentional Quit, normal session shutdown and opening an
already-running tray do not restart it. Tray recovery does not restart the
helper, master or dongles.

An unhandled Qt callback exception records its traceback and exits the tray for
recovery in a fresh UI. Errors from individual background command results remain
contained in the UI and do not stop processing subsequent results.

Runtime logs are under `%LOCALAPPDATA%\KMSwitcher\logs` on Windows, or
`$XDG_CONFIG_HOME/KMSwitcher/logs` (default `~/.config/KMSwitcher/logs`) on Linux.
`tray.log`, `tray-launcher.log` and `helper.log` include the build identity and
Python exception tracebacks, with two rotated backups and a 1 MiB limit per file.
Corresponding `*-fault.log` files retain native Python/Qt fault traces when the
runtime can capture them. Fault files rotate at the next start after reaching
1 MiB. Forced process termination or power loss may leave only the launcher's
exit-code record.

Unexpected HID-worker exceptions are logged and stop the helper cleanly;
ordinary connection failures retain automatic reconnection. An error closing
an already-disconnected device is logged and cannot prevent reconnection.

## Validation

The [desk layout functionality audit](desk-layout-audit.md) records the editor's
control-by-control coverage, regression harness, results and physical-test limits.

```powershell
python scripts/test-desktop.py
python scripts/test-core.py
python scripts/test-link-receive.py
python -m unittest discover -s companion/tests -v
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project controller
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project receiver
```

The C suite exercises production codecs, four-direction selection, monitor
seams, offset edges, target monitor selection, skipped stations, handoff timeout,
stale sessions/generations and transactional configuration. Companion tests cover
dynamic geometry, held-input exclusion, arrival idempotency, local preferences
and protocol interoperability with the compiled C master core.

Physical acceptance must exercise the complete three-station setup: laptop
swaps between powered docks, lid/monitor changes, disabled/quitting helpers,
locked/sleeping PCs, differing DPI settings, loss of radio/USB and continued
manual switching. Builds and simulation do not establish hardware latency.
