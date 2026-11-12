# KMSwitcher task list

Updated 17 October 2026. Both keyboards and pointing devices switch successfully
between two computers. All four S2s are programmed and paired, with receivers 3
and 4 tested on the development PC ready for their future computers. See [testing.md](testing.md) for
evidence and [configuration.md](configuration.md) for configurable settings.
Setup commands are in [flashing-and-pairing.md](flashing-and-pairing.md).

## Completed and verified

- [x] Install ESP-IDF, compilers, VS Code integration, and build scripts.
- [x] Create the Git repository on `master` and connect Azure DevOps `origin`.
- [x] Push the first implementation (`6ab5ae8`).
- [x] Centralise timings, radio settings, capacities, board pins, and LED settings
      under `config/`.
- [x] Identify the S3 and first S2 chip and flash specifications.
- [x] Flash diagnostic firmware successfully on both boards.
- [x] Verify the S3 RGB LED and both boards' button/long-press detection.
- [x] Verify cryptography self-tests on both boards.
- [x] Resolve the S2 diagnostic's USB warning by using TinyUSB CDC.
- [x] Confirm S2 secure boot and flash encryption are disabled.
- [x] Preserve diagnostic source separately in the repository.
- [x] Pass both production firmware builds, 63 core checks, and 10,000 malformed
      HID descriptor cases.
- [x] Add stale-clock and clock-wrap regression coverage; the expanded core suite
      passes 69 checks and 10,000 malformed HID descriptor cases.
- [x] Add HID array-release regression coverage; the expanded suite passes 80
      checks and 10,000 malformed HID descriptor cases, and both chip builds pass.
- [x] Expand core coverage for configurable triggers, save/cancel handling,
      recovery/reset gestures and blue confirmation patterns; both builds pass.

## Implemented features

These entries record implementation. The hardware checklist below records which
behaviours have been tested and which scenarios remain.

- [x] Parse generic HID descriptors for keyboard, mouse, media, and system keys.
- [x] Merge multiple input devices into one selected computer's input.
- [x] Implement double-Ctrl selection, keys 1-7, the three-second timeout, and
      retained Ctrl-tap replay.
- [x] Implement encrypted ESP-NOW transport, acknowledgements, retries, and
      duplicate rejection.
- [x] Implement button pairing and persistent slot/key storage.
- [x] Implement S2 USB HID output and input release on switching or link loss.
- [x] Add a persistent user-selectable double-tap trigger: Ctrl, Alt, Shift,
      Scroll Lock, Insert, Pause/Break or Num Lock; current trigger, C, then a
      press/release chooses it. Escape/timeout cancels without leaking commands.
- [x] Add trigger-only recovery (three short S3 BOOT presses) and a startup
      factory-reset hold confirmed by release; timing and feedback are configurable.
- [x] Make pairing and trigger-recovery feedback blue. Default the normal S3 RGB
      indication to off with `KMS_LED_NORMAL_ENABLED`; temporary feedback remains.
- [x] Implement physical keyboard LED feedback, merged into `master` by PR 311:
      S2 HID output capture, selected-PC radio return path, generic S3 output,
      retries/refresh and asynchronous disconnect-safe USB transfers. See the
      [compatibility and validation record](keyboard-led-feedback.md).

## Hardware validation: next tasks in order

- [x] Establish the S3 USB-to-UART connection for production logs, leaving native
      USB available for its host role.
- [x] Flash production switcher firmware onto the S3 and first S2.
- [x] Confirm the S2 enumerates as keyboard/mouse HID, including consumer and
      system-control collections and a serial derived from its MAC.
- [x] Test pairing, encrypted communication, and console selection using neutral
      input snapshots and application acknowledgements.
- [x] Restart the S2 and verify retained pairing, outage detection, USB readiness,
      and successful console reselection.
- [x] Restart the S3 and verify retained pairing and automatic startup selection.
- [x] Confirm the externally powered hub enumerates on the S3 native USB host port.
- [x] Connect one input receiver through the hub and verify its HID interfaces
      (K830 Unifying: keyboard and pointer reports decode).
- [x] Verify actual Windows typing and pointer movement through the S3/radio/S2
      path using the K830 keyboard and touchpad.
- [x] Test scrolling and left/right clicking on both pointing devices, plus the
      M220 scroll-wheel click.
- [x] Retest volume keys on both keyboards after the HID array-release fix;
      the user confirmed both now behave normally, including release after hold.
- [x] Test both Logitech receivers simultaneously through the powered USB hub:
      K295/K830 typing and M220/K830 pointer movement work in Windows.
- [x] Test double-Ctrl then 1 on both keyboards: the selection digit is consumed,
      typing continues, and an ordinary Ctrl+A selects text normally.
- [x] Observe timeout replay at the PC using the retained filtered Raw Input
      diagnostic: two complete Ctrl taps, followed by normal digit forwarding.
- [x] Test Escape cancellation and selecting an unpaired slot; switch keys are
      suppressed and ordinary input continues on computer 1.
- [x] Test a held selection key and mouse button during switching: holding 2
      does not type on computer 2, and a held mouse button stays released on the
      new destination until physically released and pressed again.
- [x] Identify and program a second S2 (`02:00:00:00:20:04`).
- [x] Pair the second S2 as computer 2; both paired receivers report USB ready.
- [x] Verify both keyboards and both pointing devices follow selection between
      computer 2 and computer 1, with input reaching only the selected PC.
- [x] Identify, program, and pair receivers 3 and 4; verify typing and pointer
      movement through each on the development PC, then label/store them for later.
- [x] Add a flashing/pairing guide and root-level flashing commands; use the
      receiver wrapper to build, identify, flash, and verify receiver 4.
- [x] Install merged lock LED firmware on the S3 and all four S2s, preserving
      pairings and Insert. Verify K295 Caps Lock indication from both keyboards,
      opposite states across two PCs, and both pointing devices after the update.
- [ ] Test switching between all four computers without USB reconnection.
- [ ] Test unplugging devices, powered-off PCs, radio loss, USB stalls, and recovery.
- [x] Investigate receiver 3 through the USB-C charge/display/USB adapter:
      a USB extension restores typing/pointer input and steady radio status.
      Keep the S2 away from the adapter; local interference/antenna placement is
      the leading explanation, with the exact noise source unconfirmed.
- [ ] Fix startup activation when a brief S3 reset leaves the selected S2's old
      input lease live. Reproduced during trigger-persistence testing: startup
      selected the same receiver 3 at 672 ms, timed out at 753 ms, then selected
      another available slot. Explicit reselection after the lease expires works.
      Retain the receiver's old-session protection while improving master retry.
- [ ] Measure latency, range, and prolonged-use reliability.
- [ ] Test BIOS/boot-protocol compatibility.
- [x] Verify Shift/Insert trigger changes, cancellation preserving Insert, saved
      Insert after restart, triple-BOOT recovery to Ctrl, and blue/off RGB feedback
      on the physical S3/keyboards. Other trigger choices still need hardware checks.
- [ ] Visually check the blue pairing window on the updated S3.
- [ ] Exercise factory reset on hardware when clearing/re-pairing the current
      devices is convenient; native tests cover arming, cancellation and release.
- [ ] Check Scroll/Num Lock and Pause/Break report behavior on additional keyboards.

## Remaining improvements and investigation

- [ ] Improve USB input capacity diagnostics: report channel exhaustion clearly
      and distinguish it from an unsupported HID descriptor. Investigate channel
      sharing or additional host hardware if more simultaneous input receivers
      are required; the tested hub and two combo receivers already use all eight
      channels in the current S3 host stack.
- [ ] Add automatic radio channel selection at startup, using measured link
      quality across receivers. Coordinate channel changes from the S3 and keep
      the chosen channel steady during normal input forwarding.
- [ ] Add authenticated channel discovery and recovery so receivers find the S3
      after restart, link loss, or being unplugged during a channel change.
      Persist the last working channel without losing pairing keys; keep channel
      candidates and timing thresholds configurable under `config/`.
- [ ] Validate channel selection/recovery with multiple receivers, offline PCs,
      interrupted channel changes, and interference. Check held-input release,
      recovery time, and typing/mouse latency; retain the diagnostics. Continuous
      channel hopping is outside the initial scope.
- [ ] Establish whether the S2 blue LED is programmable; add pairing feedback if
      possible. Keep its GPIO disabled until the wiring is established.
      Use the retained [GPIO sweep diagnostic](../diagnostics/README.md#s2-blue-led-gpio-sweep)
      on a spare dongle to test candidate pins.
- [ ] Extend physical lock LED checks to Num/Scroll Lock, direct-PC keyboards,
      inactive-PC changes, wake/reconnect, radio loss and unsupported reports.
      Caps Lock control from both keyboards and opposite-state two-PC switching
      have passed; all five boards now run protocol v2.
- [ ] Add individual forget/change-slot workflows; S3-wide factory reset exists.
- [ ] Investigate unreliable S2 flash readback. Verified flashing already works;
      further backup experiments are not a prerequisite for functional testing.
- [ ] Validate seven-receiver capacity when enough hardware is available.

## Slave diagnostics and OTA management (17 October 2026)

Implementation and automated checks are separate from physical rollout. See
[the operator guide](slave-management.md) for commands and migration details.

- [x] Master console help/about and descriptions of existing commands (PR 316).
- [x] Automatically embed checked-out Git identity, `dirty` state and UTC build
      time; generate exact-binary SHA-256 manifests without manual release numbers.
- [x] Report each slave's running identity, contact age and compact health summaries.
- [x] Track USB transitions/completions/stalls, radio RSSI/errors, queue peaks,
      input lease releases, resets, uptime, minimum heap and cumulative event totals.
- [x] Persist bounded circular event/statistics history, group repeated faults,
      and expose capacities, retained/overwritten counts and the oldest record.
- [x] Download immutable retained-history snapshots through the master; verify
      chunk/snapshot CRCs and archive logs on the PC.
- [x] Clear selected-slave history in RAM/flash and invalidate snapshots/cached
      reads, preserving live counters, pairing and configuration.
- [x] Use separate paced management queues, numbered/session-bound requests,
      acknowledgements, duplicate handling, retries, progress and bounded timeouts.
- [x] Provide a pinned-Python PC helper for status, log downloads/clearing and
      firmware uploads through the master's existing serial connection.
- [x] Update one slot or all paired slots sequentially, skip matching image hashes
      by default, and support `--force` without bypassing integrity/compatibility checks.
- [x] Verify size, target chip/project, flash-readback SHA-256 and the ESP image
      before activation; use dual OTA slots, health confirmation and rollback.
- [x] Update help/about/documentation, preserve pairing/data partition offsets,
      and document the one-time wired receiver migration and wired recovery.
- [ ] Migrate a spare S2 and verify retained pairing, enumeration, status,
      history downloads/clearing, rollover, reboot persistence and storage errors.
- [ ] Hardware-test one/all/skip/force updates, an offline target, interrupted
      transfers/power, first-boot rollback, active-route restoration and input latency.
- [ ] Run prolonged-use history/heap and USB suspend/resume/lease-expiry checks.
- [ ] For later consideration: live slave log streaming or an interactive slave
      diagnostic console forwarded through the master, beyond retained-log downloads.

## Browser console, master updates and preferences (17 October 2026)

Implementation is separate from the remaining physical checks.

- [x] Provide the static browser console, manual COM-port picker, receiver UI
      actions, verified log downloads and receiver application updates (PR 320).
- [x] Add master identity/OTA through the existing UART bridge, role/chip/project
      checks, readback verification, new-boot confirmation and rollback. Preserve
      NVS/PHY/factory offsets and document the one-time wired S3 migration.
- [x] Store everyday preferences on the S3: shortcut trigger, double-tap window,
      destination-selection timeout, normal RGB indication, brightness, slot
      colours and physical keyboard lock-LED feedback. Import the old trigger.
- [x] Expose load/save/defaults through the console and browser. Validate values,
      reject stale revisions, preserve pairings/history on preference reset and
      apply changes without receiver updates.
- [x] Allow the live console to fill the browser viewport and restore its regular
      size without reconnecting or losing output; Escape also restores it.
- [ ] Perform the one-time wired master migration on a suitable board; confirm
      pairing, saved trigger/preferences and retained history survive.
- [ ] Hardware-test master skip/force/update, correct prior-destination restoration,
      a previously paused route, corrupt/interrupted transfer, cable/power loss,
      pending-boot failure/rollback and recovery through the factory application.
- [ ] Verify every preference on hardware, persistence after power loss, legacy
      trigger import, held-input release, live brightness/colour changes, keyboard
      LED disable/re-enable without reconnecting and preference-only reset.

## Advanced runtime options for a later PR

- [ ] Consider configurable recovery buffer duration (`KMS_RECOVERY_BUFFER_MS`,
      currently five seconds), and an explicit discard-versus-buffer policy.
      Retain bounded replay, stale-input expiry and held-input suppression.
- [ ] Consider configurable failure indication: flash count and on/off timing.
- [ ] Consider configurable pairing-window duration (`KMS_PAIR_WINDOW_MS`, 30 s).
- [ ] Consider diagnostic checkpoint intervals, repeated-event grouping and idle
      thresholds. Receiver-specific intervals need management configuration
      support; keep storage capacity/schema changes as firmware changes initially.

## Settings intentionally kept in firmware initially

These are engineering limits or recovery/compatibility decisions, not controls
for the everyday preferences page. Revisit only through a separately designed
change with the relevant validation.

- GPIO assignments, board wiring, LED model/polarity and USB host power handling.
- USB descriptors, interface/endpoint layout, identifiers and descriptor strings.
- Queue sizes, parser/report limits, task stacks/priorities and history capacities.
- Radio retry timing, heartbeat/snapshot intervals, input leases, peer deadlines
  and OTA transfer/boot-health timing. Their cross-setting constraints must hold.
- Radio channel and country; changing them needs coordinated receiver discovery
  and recovery. The channel-selection/discovery tasks above remain separate work.
- Physical factory-reset and trigger-recovery gestures, so the hardware recovery
  path stays predictable even when everyday preferences are changed.

## Next test setup

Keep the S3 connected through its USB-to-UART bridge and the paired S2 connected
to the PC. Identify actual Windows ports before flashing; native USB and the UART
bridge have different roles, and connector labels vary between clones. Both
Logitech input receivers now work through the externally powered hub on native
USB. Two-computer switching and the held-input checks have passed. Receivers 3
and 4 are already paired and ready to move to their intended computers; those
computers were unavailable during staging. Receiver 3 has since also been tested
through a USB-C adapter and extension. Use S3 `status` to check the current trigger,
destination and online peers before each test. Continue four-computer and
failure/recovery tests when the computers are available. When short of PC USB ports, temporarily
borrow the working S2's port for programming, move the new
receiver to its computer, return the original receiver, and reselect computer 1.
Keep this checklist updated as tests pass.


## Recovery and diagnostic rollout (17 October 2026)

- [x] Implement same-PC recovery, startup lease retry, five-second bounded RAM
      replay, a retained fault journal and configurable hourly idle/statistics
      checkpoints; native checks and both firmware builds pass.
- [ ] Install the S3 change and verify physical radio loss, receiver USB suspend/
      replug, receiver reboot, startup lease recovery, keyboard/mouse replay,
      buffer expiry and switching during recovery. Existing S2s remain compatible.
- [ ] Run an hourly-storage soak and check that saved history survives S3 restart;
      inject storage failures and confirm input continues.
