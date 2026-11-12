# Desk layout functionality audit

Audited 27 October 2026. Scope: the companion desk editor, its tray and settings
entry points, and the shared helper/protocol paths used by those controls.
The Setup and Stress Toolkit products also have separate regression suites;
their hardware workflows are outside this editor audit.

## Findings corrected

| Finding | Resulting behaviour |
| --- | --- |
| New stations all started at 0,0 | New stations use free space; the preview scales to fit every dongle. |
| Movable parent had zero bounds; redraw events were used to track coordinates | A graphics group covers every screen; position changes update X/Y immediately. Numeric edits repaint both the old and new locations. |
| Stations could not snap together | Mouse dragging snaps nearby station boundaries in four directions and aligns tops, bottoms or centres. Numeric coordinates remain exact. |
| Monitor labels spilled onto adjacent previews | Labels stay within their screen, with shortened text where needed and full text in tooltips. |
| Loading numeric fields triggered redraws halfway through a document | Loading applies the complete document before drawing; row coordinates stay intact. |
| Export showed a save dialog while disconnected, then failed because no master document was loaded | Offline drafts export successfully. Writes are atomic, missing extensions gain `.json`, and success reports the saved path. Errors have a visible dialog. |
| Invalid JSON document shapes could escape validation or partially affect the UI | The whole document and revision are validated before any controls change. UTF-8 backups with a BOM are supported. |
| Import or Reload could discard edits without confirmation | Both actions check for unsaved edits. Cancel leaves the layout intact. |
| Save or Reload responses could replace edits made while waiting | An edit generation protects newer edits; a successful save advances the revision without discarding them. A stale import is never silently rebased. |
| Connection recovery did not retry an initially failed configuration read | Reconnecting loads an untouched offline editor; edited drafts remain available to export. |
| Disconnect left old availability and topology information visible | Peer availability resets; topology polling avoids duplicate requests and defers redraws until a drag ends. |
| Preview scaling and saved-size capture assumed nonnegative monitor origins | Monitor extents are normalized consistently before preview and size capture. |

## Functional coverage

The desk harness is `companion/tests/test_layout_ui.py`. It has 28 tests,
including real Qt mouse events, actual JSON file writes, pixel checks for
numeric movement, and assertions that new previews fit without changing the
existing station geometry. It runs automatically through
`scripts/ci-tests.py python` in the existing CI pipeline.

| Area | Validation |
| --- | --- |
| Add/remove all seven dongles | Non-overlapping initial placement, valid bounds, automatic fit. |
| Move a station with multiple monitors | Real mouse dragging moves the group, synchronizes X/Y, and retains movement across a topology update. |
| Edge snapping | Left/right/above/below, edge alignment, stable scale during dragging, third-station obstruction. |
| Numeric X/Y and area width/height | Exact coordinates, negative positions, fixed and automatic sizes, old/new pixel repaint, coordinate limits. |
| Window resize and Fit layout | Every preview remains within the visible overview. |
| Export/import | File round trip, revision retention, offline draft binding, cancelled dialogs, invalid structures, BOM, missing directories, failed commit preserving an existing backup. |
| Save to master | Every layout option, automatic screen-size capture, successful revision advance, stale-revision rejection, duplicate-save guard and edits during pending operations reviewed. |
| Reload | Complete document application, unsaved-edit cancellation, late responses retaining newer edits. |
| Local/global switching | Local toggle submits immediately without dirtying the master; global enable/disable persists with layout options. |
| Availability/recovery | Disconnect reset, untouched-editor reconnect, duplicate topology-read prevention, refresh deferred during drag. |
| Login startup | Success routes to the startup implementation; errors restore the checkbox and display the failure. The test does not change real login settings. |
| Device and clipboard preferences | Dialog edits submit the chosen values and expected device revision. |
| Setup/About/close/reopen/quit | Correct setup entry point; version dialog; tray close/reopen; unsaved-work checks before quitting. |
| Clipboard operations | Existing companion tests cover Qt text/rich text/image/file capture and publication, tray transfer actions, encrypted loopback transport and service error cases. |
| Shared helper and firmware | Existing agent, IPC and native-protocol tests exercise command routing, persisted config, stale revisions, geometry, readiness and screen-edge routing. |

## Running the checks

Run the focused harness from the repository root after installing the companion
UI dependencies:

```powershell
$env:PYTHONPATH = 'companion;linux'
$env:QT_QPA_PLATFORM = 'offscreen'
python -m unittest discover -s companion/tests -p test_layout_ui.py -v
```

The full Python run needs the repository's build/test dependencies and permits
local loopback sockets for the IPC and encrypted clipboard integration checks:

```powershell
python scripts/test-desktop.py
python scripts/ci-tests.py python
python scripts/ci-tests.py native
```

## Remaining physical acceptance

Automated validation uses real Qt widgets and production protocol code with
simulated devices. It does not establish live hardware behaviour. Check the
installed build with connected dongles on Windows and Linux, radio/USB loss,
locked/sleeping computers, actual mixed-DPI monitors, and real login startup.
The setup route was exercised without flashing devices. Native save-file dialogs
and OS permission prompts were replaced in the harness; the selected paths were
written and read through the real file implementation.
