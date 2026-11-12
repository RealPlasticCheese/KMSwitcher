# Computer power reporting

Run the companion on each computer and update the S2 receivers and S3 controller
from the matching firmware bundle. The desk layout's **PC power** column and
`kmswitcher --json peers` show each computer's last companion report. Local
`kmswitcher --json status` also includes `power_state`, `power_fresh`,
`power_supported` and `power_notifications`.

| Report | Meaning |
| --- | --- |
| Awake | The helper is running and renewing its report. This includes a locked desktop. |
| Sleep / hibernate requested | The OS notified the helper that it is suspending. These notifications do not identify sleep versus hibernation. |
| Shutdown / restart requested | The OS notified the helper of shutdown or restart. This is intent, not proof that power is off. |
| Unknown | No report is available, the helper stopped normally, notifications are unavailable, or firmware needs updating. |

Reports become **Last reported** after three seconds without renewal. The JSON
fields include freshness and age; an old Awake report must not be interpreted as
proof that the computer is still running. A crash, cable removal, forced shutdown
or loss of power may prevent the final notification. Receiver reboot, USB
reattachment and a new helper session discard the receiver's old report.

The background helper receives Windows suspend/resume and shutdown/cancellation
notifications through a hidden window; the tray does not need to be open. Linux
uses logind's system-bus notifications and a delay inhibitor, released immediately
after a bounded reporting attempt. It does not prevent sleep or change permanent
power settings. If logind or the necessary permissions are unavailable, status
explains this and the state remains Unknown. Reporting works with automatic edge
switching disabled and with the desktop locked.

## Mouse jiggler

The receiver already stops all jiggler output while USB is suspended or unmounted.
The companion's sleep/hibernate or shutdown report additionally pauses jiggling
before USB suspension. This pause is retained even after the report becomes stale;
an Awake report on resume (or a new USB/helper session) clears it. The enabled
setting, interval, distance and mode are preserved. While paused, the idle timer
is refreshed, so the next jiggle waits the configured interval after resume.

Ordinary routed key/mouse input can still request USB remote wake when the host
allows it. Neither the jiggler nor selecting a dongle requests remote wake.

Older firmware continues to support existing companion functions. An older S2
does not receive the new command; an older S3 cannot display remote PC reports.
USB-suspend protection still applies without a companion. A powered USB port alone
cannot establish whether its computer is awake, asleep, hibernated or shut down.
