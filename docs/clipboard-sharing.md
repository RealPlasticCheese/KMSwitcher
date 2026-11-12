# Send clipboard content to another computer

Copy normally, then right-click the KMSwitcher companion tray icon and choose
**Send clipboard to Dongle 2 — Computer name** (or another destination). Each
destination is directly in the root menu. The dongle identifies the desk station;
the name identifies the computer currently attached there. You can also choose a
destination from **Send clipboard** on the **Transfers** tab in Settings.
Unavailable destinations are disabled.

## Send automatically when you copy

In **Settings → Clipboard**, use the **Send automatically** dropdown for each
content type. **Off** is the default and keeps that type available through
manual sending. Choose **Selected computers** to use the shared row of
destination checkboxes, or choose **All available paired computers**.
Click **Save clipboard settings**. The choices apply to copies made on this
computer. Enable them on both computers to share in both directions.
An existing shared mode becomes the starting mode for each type.
Only enable automatic sending to destinations you trust with the content you copy.

The destination row shows unavailable checkboxes for stations waiting for a
compatible companion. Start the tray app on each destination and update its
companion if needed.

Automatic sending supports plain text, formatted text, images, and files and
folders. It uses the same transport and size limits as manual sends. Update
the companion on each participating computer. Selections follow the computer
between dongles. Unavailable or locked computers are skipped for that copy.
A newer copy replaces pending sends, while a transfer already in progress
can finish. Each recipient gets the same captured content and applies its own
receiving settings.

Update the controller, receivers and companions together. Reconnect each receiver
after updating so the OS reads its new USB report descriptor. Older firmware
still supports its existing features but cannot participate in clipboard transfers.
Both computers need the tray app running in an unlocked supported desktop session.
The existing Windows and Linux X11 desktop backends are supported; Wayland remains
unavailable. Turning off automatic screen-edge switching does not disable sending.

## Receiving and pasting

Open **Settings → Clipboard** to choose receiving behaviour independently for
**Plain text**, **Formatted text**, **Images**, and **Files and folders**:

- **Put on clipboard automatically** (or **Prepare for paste automatically** for
  files): after verification, replace the destination clipboard and paste normally.
  Plain text and formatted text use this by default.
- **Accept manually in Transfers**: leave the clipboard untouched until you choose
  **Put on clipboard** or **Prepare for paste**. Images and files use this by default.
- **Save automatically to folder**: choose an **Automatic save folder**. Received
  items are saved there without replacing the clipboard or opening the file manager.
  If a save fails, use **Save to…** to retry.

Use the **Show arrival popup** checkbox beside each receive dropdown to choose
which content types show a tray popup. It is checked by default. The popup
appears when the item is ready to paste, waiting in Transfers, or saved to a
folder, depending on the receive setting.

Click **Save clipboard settings** to apply these choices. Supported formats are
plain text, HTML, RTF and PNG (native clipboard images are converted to PNG).
Items containing both an image and formatted text use the Images preference;
local file references use the Files and folders preference.

Open **Clipboard transfers…** from the tray, or **Settings → Transfers**, for
individual actions:

- **Text, formatted text and images:** choose **Save to…** and a directory. Files
  use a local timestamp such as `20261104-090807-123456.txt`, `.html`, `.rtf` or `.png`.
  All supported representations are saved beside each other with the same timestamp.
  Existing files are never overwritten.
- **Files and folders:** choose **Save to…** to create a new `KMSwitcher-xxxxxxxx`
  folder under the selected directory, or **Prepare for paste** to put the received
  local file references on the clipboard. Paste normally in Explorer or the Linux
  file manager. File-manager conventions differ; Save to works independently of
  those conventions. Source files are copied, never moved or deleted.
  After saving, the file manager opens with the new `KMSwitcher-xxxxxxxx` folder
  selected. Use **Open location** in that transfer's **Actions** column to reveal
  it again. If the Linux file manager does not support selection, its parent
  directory opens instead. The button becomes available only after a successful
  save and stays available until the transfer is dismissed or the helper restarts.

Manual acceptance and saving leave the current clipboard untouched. Private
application formats are not transported; supported representations are retained.
Files/folders, including empty directories, are captured into a fixed snapshot
before sending starts. Files that change during capture are rejected. Copying
something else afterwards does not alter the snapshot.

The Transfers tab shows each transfer's ID, computer, date and time, byte progress,
measured transfer rate, current transport, status, errors and cancellation. The
ID matches the staging folder name for incoming transfers. **Clear All** clears
the list and staged transfers. Use the other **Clear** buttons to remove only
plain text, formatted text, images, or files and folders. Each asks for confirmation.
Completed sends and incoming items can also be dismissed individually.
**View** opens staged text, formatted text or images in the default app for their
file type. **Open folder** goes straight to the extracted files, which have names
such as `Clipboard.txt` and `Clipboard.png`. For a transfer with one text,
PDF, image, audio or video file, **View** opens it in its default app. Other
file and folder transfers open in the file manager. If staging expires after
a save, **View** can open the saved copy.
**Open transfer folder** opens the local staging directory in Explorer (or the
default Linux file manager). On Windows this is
`%LOCALAPPDATA%\KMSwitcher\clipboard`; Linux uses
`${XDG_CONFIG_HOME:-~/.config}/KMSwitcher/clipboard`. A growing incoming
`bundle.zip` is incomplete until verified. Saved copies are separate from staging.
Outgoing snapshots are removed after the send job finishes.
Either computer can cancel an unfinished transfer, including an incoming item
that reports an error. Cancel notices use the paired dongles for both content
transports. Progress updates keep the action buttons available while clicked.
If verification finishes before cancellation arrives, the received content remains
available to accept or save.
Prepared files must remain in staging for subsequent Paste to work; **Remove staged
files…** explicitly releases them after confirmation. Unsaved received items and
staged files are cleared when the background helper restarts. Save important files
before quitting. The clipboard snapshot is not a clipboard history or backup.
Automatically placed text/images remain available for **Save to…** in a bounded
cache until dismissed, ten minutes elapse, or newer receives need the space.
This does not remove the content already placed on the OS clipboard or saved files.

## Transport preferences belong to the computer

Open **Settings → Clipboard** to set the computer name, maximum item size and:

| Preference | Allowed content path |
| --- | --- |
| Dongle only | Encrypted paired dongles |
| Network preferred, fallback to dongle | Authenticated encrypted TCP first, dongles if unavailable/interrupted |
| Network only | Authenticated encrypted TCP; no dongle content fallback |

Both computers must allow the selected path. Dongle only against network only is
incompatible. Network preferred against dongle only uses dongles. These preferences
are in the local computer's `clipboard.json`, separate from the S3 layout and
device preferences, and follow a laptop between docks.

Companions always use the paired encrypted dongle control path to discover current
helper sessions and exchange ephemeral credentials specific to each pair of computers, even in network-only
mode. Content transport preferences apply to the clipboard payload. An old IP
address cannot identify a computer after a dock swap or helper restart.

LAN uses directly reachable IPv4 addresses and a dynamically selected TCP listening
port. Permit the installed companion on your private network if the OS firewall
asks; no firewall settings are changed by the app. Up to four local IPv4 addresses
are advertised. VLAN isolation, VPN routing or firewall rules can prevent a direct
connection. Network-preferred mode then falls back to the dongles automatically.
Network-only mode reports the failure. Nothing uses an internet server.

The default maximum is 256 MiB per item (configurable from 1 to 2048 MiB on each
computer). Both the packed transfer and unpacked content must fit the receiving
limit. Up to eight incoming items and twice the configured item limit may be staged;
dismiss items to free space. Bundles reject unsafe/ambiguous paths, links/junctions,
unsupported MIME formats and more than 10,000 source entries. Destination files are
never overwritten by Save to.

## Transfer protocol

Clipboard traffic has its own USB feature reports (34 send, 35 receive), bounded
queues and management operation 12. A report is 192 bytes: a 28-byte header,
160-byte payload and CRC32. EP0 splits a report into normal full-speed USB packets;
keyboard/mouse interrupt endpoints and the 64-byte desktop command protocol stay
unchanged. Receiver status advertises the clipboard capability in data byte 44.

Packets bind both station numbers and current helper sessions. The master checks
both live leases and the authenticated source receiver before forwarding. The
existing encrypted paired unicast carries radio packets. Input and desktop/OTA
management have precedence over clipboard traffic; the existing 10 ms management
pacing remains conservative. Per owner-loop iteration, the companion reads at most
four queued reports, checking a 2 ms receive budget between reads, and sends at
most one clipboard packet. Empty queues end the read burst immediately. Active
dongle sends refresh their bound peer directly, without blocking the HID owner
on synchronous station-discovery probes. Firmware holds only bounded packet queues,
never whole images or files. The sender keeps up to four 160-byte content chunks
in flight instead of waiting for each chunk's full round trip. Replies acknowledge
the contiguous received offset, so a later reply can cover a lost acknowledgement.
Pending DATA acknowledgements for the same transfer coalesce to the latest
contiguous offset; no batching delay is added. BEGIN/END/CANCEL and error replies
remain individually addressable. The acknowledgement cache is bounded separately,
and acknowledgement/output queues take turns when both have traffic.
If progress stalls, the sender retries the outstanding window from its oldest
unacknowledged chunk. Retries back off from 0.8 to 1.6 to 2 seconds, capped at
2 seconds. Thirty seconds without acknowledged progress ends the transfer;
cancellation remains available while waiting. BEGIN/END exchanges use the same
backoff and timeout. Final SHA-256 and bundle validation still gate acceptance.
Incoming chunks use one buffered staging-file handle per transfer. It is flushed
and closed before final hashing, and closed on cancellation, dismissal, expiry or
shutdown. The growing file's on-disk size can lag the accepted-byte progress until
the write buffer is flushed.

Keyboard/mouse traffic can delay low-priority clipboard discovery as well as
content. New transfers require fresh discovery. An established transfer may
continue with the same bound source/destination sessions while discovery is
delayed, within its ten-minute inactivity limit. The receiver still rejects a
changed session, a locked/unavailable local desktop or a disallowed transport.

The companion uses a shared bounded stream sender for both dongle and LAN paths.
Updated receivers expose available USB buffer slots, so the companion pauses
submission when the dongle is full. New status snapshots also report queued incoming
cells, avoiding empty reads and a separate USB capacity poll. Credit-capable devices
use bounded multi-cell bursts so slow USB calls do not force one cell per helper
cycle. New firmware negotiates a radio batch envelope
and combines up to four data cells into one ESP-NOW v2 transmission. Older
clipboard-capable firmware uses the existing single-cell wire protocol.

Choose **Dongle speed** in **Settings → Clipboard**. Conservative is the default;
Balanced and Fast use higher radio rates on negotiated links. The transfers window
shows the requested profile and the effective profile for the local radio hop.
Repeated radio failures step the affected link down; input retains priority.
See [Shared stream transport](stream-transport.md) for profile settings, OTA use,
memory budgets and compatibility.
Physical speed still depends on radio conditions, keyboard/mouse activity and
host USB/companion scheduling; the window is not a guaranteed transfer rate.

LAN exchanges use 64 KiB chunks, directional AES-256-GCM keys, fresh random client
and server challenges, ordered records and a strict record-size bound. Keys derive
from the two ephemeral credentials exchanged over the paired dongles and the current
source/destination sessions. The listener accepts at most two concurrent connections.
Disk/ZIP and TCP work run separately from the HID owner. The destination validates
the completed bundle before exposing it to the tray. Automatic text classification
is derived from the validated formats, not the sender's claimed content type.

Network interruption can resume the same transfer over dongles from the last
acknowledged offset. A completed transfer is committed once even if its final reply
is lost. A helper/session change aborts the transfer instead of delivering to a new
computer at the same dock. Partial incoming transfers expire after ten minutes.

## Validation and physical acceptance

Run `python scripts/test-desktop.py`, `python scripts/test-core.py`,
`python scripts/test-link-receive.py` and
`python -m unittest discover -s companion/tests -v` with companion dependencies
installed. Production controller and receiver builds use `scripts/idf.ps1 -Release`.
The companion tests cover real encrypted loopback TCP, Qt text/image/file MIME
publication, radio loss/retries, resumption, integrity and transport preferences.
`test_clipboard_radio.py` also compares the old sender and the four-chunk window
over the same deterministic link model (120 ms round trips and 10 ms packet
pacing). It exercises the production codec, sender and receiver, checks saved file
contents after loss/reordering, and bounds pending chunks and retries. Its timing
comparison measures avoided protocol waiting rather than physical radio speed.
It also models ten seconds of input congestion without discovery updates and
checks recovery, progressive backoff, prompt cancellation and a bounded timeout.
Additional checks cover cumulative ACK coalescing, bounded receive bursts, active
peer discovery without blocking probes, and buffered-handle cleanup and integrity.

These checks do not establish radio throughput or physical keyboard/mouse latency.
On the desk, test each content type in both directions between Windows and Linux,
all three transport policies, blocked LAN and mid-transfer disconnection, laptop
dock swaps, locked PCs, cancellation and saved/prepared files. Measure elapsed time
for representative text, a screenshot and a binary file while using the mouse and
keyboard continuously. The current radio path intentionally prioritizes input;
large transfers should normally use LAN. No MB/s radio claim is made before these
hardware measurements.
