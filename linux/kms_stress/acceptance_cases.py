"""Physical rig procedures and required observations for each acceptance case."""

CASES = {
    "contention-latency": {
        "steps": (
            "Use three PCs. Capture steady keyboard and pointer input to a healthy active dongle.",
            "Impair a second, inactive dongle with controlled attenuation. Repeat with real Companion "
            "clipboard advertisements, a transfer, and receiver status/history reads in progress.",
            "Capture the first fresh input after repeated handoffs. Use one acquisition clock for "
            "source and destination events in each window.",
        ),
        "checks": {
            "contention-observed": "The inactive link showed slow or failed transmissions while the active host was receiving input.",
            "concurrent-traffic": "Logs show actual clipboard and management traffic during input capture.",
        },
    },
    "radio-recovery": {
        "steps": (
            "Use everyday mode with automatic channel and receiver recovery enabled. Run foreground input throughout.",
            "Flap one inactive link, then impair two links long enough to qualify a trial. Exercise a "
            "returning late peer and repeat with only one reachable peer.",
            "Exercise eligible receiver restart and a failed confirmation. Record channel, boot IDs, "
            "selection, trial/rollback messages and the recovery budgets.",
        ),
        "checks": {
            "single-peer-isolated": "A lone inactive fault leaves the healthy route and current channel working.",
            "trial-and-rollback": "A viable trial retains the channel and rejoins peers. A failed trial rolls back and restores input.",
            "restart-guards": "Eligible recovery targets only the affected dongle, respects cooldown/budget, and preserves manual route changes.",
        },
    },
    "desk-switching": {
        "steps": (
            "On installed Windows and Linux Companions, use the physical double-tap shortcut and actual "
            "screen-edge switching while typing and moving the pointer.",
            "Exercise rapid reversals, held modifiers/buttons, mixed-DPI monitors, lid or monitor changes, "
            "and a sleeping or unreachable intermediate station. Check the supported desktop backends.",
        ),
        "checks": {
            "shortcut": "The trigger and digit are consumed, and the first fresh input reaches the selected PC.",
            "edge": "The pointer lands on the correct monitor and the Companion skips unavailable stations as configured.",
            "isolation": "The old route releases held input and fresh input reaches only the destination.",
        },
    },
    "host-power": {
        "steps": (
            "On Windows and Linux, lock, sleep, restart and shut down both selected and inactive PCs. "
            "Switch away during shutdown and try reselecting after the host returns.",
            "Repeat wake with host wake permission on and off. Try a key, mouse, selection alone, a "
            "neutral release and the receiver jiggler. Check keyboard input and release in BIOS boot protocol.",
        ),
        "checks": {
            "route-during-outage": "Switching away works during shutdown and returning hosts receive fresh input only.",
            "wake": "Eligible input wakes the sleeping host only when permitted, with no stale input replay on resume.",
            "boot-protocol": "Firmware setup accepts keyboard navigation and all released keys clear.",
        },
    },
    "usb-hotplug": {
        "steps": (
            "Use K830/c52b and K295/M220/c534 through a powered hub. Record both interfaces per input "
            "and all eight host channels in use.",
            "During input and lock-LED updates, unplug each input separately and interrupt hub power. "
            "Repeat immediate and delayed reconnection with both input devices active.",
        ),
        "checks": {
            "sibling-progress": "Removing one input preserves progress and releases controls from only the removed device.",
            "reenumeration": "Returning inputs reclaim the expected interfaces and resume without repeated allocation faults.",
            "hub-and-leds": "After a hub interruption all input returns neutral and lock LEDs follow the selected host.",
        },
    },
    "clipboard": {
        "steps": (
            "Use the packaged Windows and Linux Companions. Transfer text, an image and a large file in "
            "both directions with LAN-only, dongle-only and LAN-preferred policies.",
            "Block LAN before and during a transfer. Repeat with simultaneous transfers, active input "
            "and a cancellation from each end. Retain transfer summaries and source/destination hashes.",
        ),
        "checks": {
            "payload-integrity": "Content and file hashes match in both directions with a single completed delivery per transfer.",
            "fallback-and-cleanup": "LAN-preferred resumes over dongles and cancellation clears staging and the active transfer.",
            "input-during-transfer": "Keyboard and pointer stay responsive throughout transfer, interruption and cleanup.",
        },
    },
    "companion-uptime": {
        "steps": (
            "Leave packaged Companions running for the planned soak with regular clipboard transfers and topology changes.",
            "Cycle dock USB, disconnect the receiver, lock/sleep/resume, and restart the Windows shell "
            "or re-login on Linux. Exercise one abnormal tray exit and one intentional quit.",
        ),
        "checks": {
            "reconnect": "The tray and HID owner remain usable or recover, and fresh input and transfers resume.",
            "supervision": "Unexpected exit is recorded and recovery is bounded. Intentional quit remains closed.",
        },
    },
    "mixed-soak": {
        "steps": (
            "Run the Mixed soak preset after Direct baseline. Set a long quiet interval and independent "
            "generator rates within verified fixture capture capacity.",
            "Repeat at the intended sustained rate on every enabled receiver. Attach result.json and "
            "record controller/receiver builds and the measured capture headroom.",
        ),
        "checks": {
            "throughput": "Both sources sustain the planned rates on each receiver with no unexplained input or capture loss.",
            "idle-and-continuous": "Each receiver has one uninterrupted input window after the planned idle, with a clean neutral release.",
        },
    },
    "ota": {
        "steps": (
            "On a spare rig with a known-good image, run input during master and receiver update preparation.",
            "Interrupt upload and activation/reboot separately. Exercise a failed boot confirmation and "
            "an all-receiver update with an offline target. Retain image hashes and boot/route evidence.",
        ),
        "checks": {
            "safe-routing": "Update guards prevent unsafe selection or replay, and input resumes on the intended route.",
            "rollback": "Incomplete images never activate and failed boot confirmation rolls back to the expected image.",
            "retained-state": "Pairing and preferences survive and the offline batch target is reported separately.",
        },
    },
}
