"""Radio controls use the helper's existing receiver session."""
from .protocol import Op, read_words, words
from .transport import DeviceError, RejectedCommand

MIN_COOLDOWN, MAX_COOLDOWN = 5, 60
RECOVERY_PHASES = ("Monitoring", "Announcing a trial", "Verifying the new channel",
                   "Returning to the previous channel", "Waiting for dongles to reconnect")
RECOVERY_RESULTS = ("", "Kept the new channel", "Returned to the previous channel", "Cancelled by a manual action",
                    "Channel trial failed", "Replaced by a manual channel change", "Interrupted by radio driver recovery",
                    "Could not save recovery state")


def _recovery_settings(args):
    enabled, single, cooldown = (args.get(key) for key in ("enabled", "single_peer", "cooldown"))
    if (type(enabled) is not bool or type(single) is not bool or
            type(cooldown) is not int or not MIN_COOLDOWN <= cooldown <= MAX_COOLDOWN):
        raise ValueError(f"Set a recovery cooldown from {MIN_COOLDOWN} to {MAX_COOLDOWN} minutes")
    return bytes((1, enabled | (single << 1), cooldown))


def _recovery_status(data):
    if len(data) < 12:
        raise ValueError("Incomplete radio recovery status")
    schema, flags, cooldown, phase, result, old, candidate, reason, expected, affected, attempts, limit = data[:12]
    if (schema != 1 or flags > 3 or not MIN_COOLDOWN <= cooldown <= MAX_COOLDOWN or
            phase >= len(RECOVERY_PHASES) or result >= len(RECOVERY_RESULTS) or
            old > 13 or candidate > 13 or reason > 2 or expected > 127 or affected & ~expected or
            limit != 2 or attempts > limit or any(data[12:])):
        raise ValueError("Invalid radio recovery status")
    return dict(enabled=bool(flags & 1), single_peer=bool(flags & 2), cooldown=cooldown,
                phase=phase, result=result, old=old, candidate=candidate, reason=reason,
                expected=expected, affected=affected, attempts=attempts, limit=limit)


def request_channel(receiver, command, args):
    operations = {"channel-get": Op.CHANNEL_GET, "channel-set": Op.CHANNEL_SET,
                  "channel-resume": Op.CHANNEL_RESUME, "channel-recovery-get": Op.CHANNEL_RECOVERY_GET,
                  "channel-recovery-set": Op.CHANNEL_RECOVERY_SET}
    recovery = command.startswith("channel-recovery-")
    data = b""
    if command == "channel-set":
        channel = args.get("channel")
        if type(channel) is not int or not 1 <= channel <= 14:
            raise ValueError("Select a valid radio channel")
        data = words(channel)
    elif command == "channel-recovery-set":
        data = _recovery_settings(args)
    try:
        reply = receiver.request(operations[command], data)
    except RejectedCommand as error:
        if str(error) == "unsupported firmware":
            if recovery:
                raise DeviceError("Update the master and local dongle firmware to use automatic channel recovery") from error
            raise DeviceError("Update every paired receiver and the master before changing channels") from error
        raise
    except DeviceError as error:
        if recovery:
            raise DeviceError("Recovery settings were not acknowledged. Check the master connection and firmware, then refresh before retrying.") from error
        raise DeviceError("Radio command was not acknowledged. Check the master connection and update both firmware roles if needed. Refresh status before retrying a change.") from error
    if recovery:
        return _recovery_status(reply.data)
    current, pending, first, count, paired, supported = read_words(reply.data, 6)
    if not 1 <= first <= current < first + count <= 15 or not first <= pending < first + count or not 0 <= supported <= paired <= 7:
        raise ValueError("Invalid radio channel status")
    return dict(current=current, pending=pending, first=first, count=count,
                paired=paired, supported=supported)
