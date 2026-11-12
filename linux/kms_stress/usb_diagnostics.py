"""Bounded USB snapshots and symptom classification, without inferring a cause."""
import re


def fields(line, hexadecimal=()):
    result = {}
    for key, value in re.findall(r"([a-z][a-z0-9-]*)=([^ ]+)", line):
        if key in result: raise ValueError(f"Duplicate USB field {key}")
        if re.fullmatch(r"[0-9a-fA-F]+", value):
            number = int(value, 16 if key in hexadecimal else 10)
            if not 0 <= number <= 0xffffffff: raise ValueError("USB field outside uint32")
            result[key] = number
        elif re.fullmatch(r"\d+/\d+", value):
            numbers = list(map(int, value.split("/")))
            if any(n > 0xffffffff for n in numbers): raise ValueError("USB ratio outside uint32")
            result[key] = numbers
        else: raise ValueError(f"Malformed USB field {key}")
    return result


def newer(value, previous):
    return 0 < ((value - previous) & 0xffffffff) < 0x80000000


def snapshot(lines, token):
    if len(lines) > 512: raise ValueError("USB status exceeds bounded frame")
    begin, state, counts, timing, end = None, None, None, None, False
    retained = []
    for line in lines:
        if line.startswith("KMSUSBSTATUS BEGIN "):
            item = fields(line, ("boot",))
            if begin is not None and not end: raise ValueError("Interleaved USB status frame")
            if item.get("id") != token: continue
            if begin is not None: raise ValueError("Duplicate USB status frame")
            begin = item
        if begin is None or end: continue
        retained.append(line)
        if line.startswith("KMSUSBSTATUS STATE "):
            if state is not None: raise ValueError("Duplicate USB state")
            state = fields(line)
        elif line.startswith("KMSUSBSTREAM COUNTS "):
            if counts is not None: raise ValueError("Duplicate USB counts")
            counts = fields(line)
        elif line.startswith("KMSUSBSTREAM TIMING "):
            if timing is not None: raise ValueError("Duplicate USB timing")
            timing = fields(line)
        elif line == f"KMSUSBSTATUS END id={token}": end = True
    if (not end or begin is None or not {"id", "boot", "at", "slot", "fault"} <= begin.keys() or
            state is None or not {"state", "attempts", "recovered", "restarts", "controls", "hubs", "inputs", "capacity", "suspended", "pending"} <= state.keys() or
            counts is None or not {"completed", "dropped", "expired", "overflows", "last-complete"} <= counts.keys() or
            timing is None or not {"callback-us", "callback-at", "dispatch-us", "dispatch-at", "poll-gap-us", "gap-at", "decode-us", "decode-at"} <= timing.keys()):
        raise ValueError("Incomplete/token-mismatched USB status; update the test-controller firmware")
    if state["controls"] + state["hubs"] + state["inputs"] > state["capacity"] or not 0 <= begin["slot"] <= 7:
        raise ValueError("Inconsistent USB status")
    return {**begin, "state": state, "counts": counts, "timing": timing, "lines": retained}


def fault_frames(lines):
    """Ignore incomplete windows, including an overwritten beginning."""
    result, frame = [], None
    for line in lines:
        if line.startswith("KMSUSBFAULT BEGIN "):
            header = fields(line, ("boot",))
            match = re.match(r"KMSUSBFAULT BEGIN (\d+) ", line)
            if not match: frame = None; continue
            if not {"boot", "at", "slot", "op", "target"} <= header.keys():
                raise ValueError("Incomplete USB fault header")
            sequence = int(match[1])
            if not 1 <= sequence <= 0xffffffff: raise ValueError("Invalid USB fault sequence")
            frame = {**header, "sequence": sequence, "samples": [], "qtd": [], "pending": [], "lines": []}
        if frame is None: continue
        frame["lines"].append(line)
        if len(frame["lines"]) > 256: frame = None; continue
        if line.startswith("KMSUSBFAULT BUS "):
            frame["bus"] = fields(line, ("hfnum", "hprt", "gintsts", "gintmsk", "haint"))
        elif line.startswith("KMSUSBSTATE SAMPLE "):
            frame["samples"].append(fields(line, ("frame", "ahb", "reset", "nptx", "ptx", "host", "clock", "core", "mask", "channels")))
        elif line.startswith("KMSUSBTRACE QTD "):
            match = re.search(r"current=([0-9a-fA-F,]+)", line)
            if match:
                values = [int(v, 16) for v in match[1].split(",")]
                if len(values) != 3 or any(v > 0xffffffff for v in values): raise ValueError("Invalid USB QTD status")
                frame["qtd"].extend(values)
        elif line.startswith("KMSUSBFAULT PENDING "):
            # Setup bytes and signed status/byte counts are retained verbatim.
            frame["pending"].append(line)
        elif line == "KMSUSBFAULT END":
            result.append(frame); frame = None
    return result


def classify(before, after, frozen, item):
    errors = item.get("errors", [])
    allocation_failure = any(event.get("event") == "USB endpoint allocation failed" for event in item.get("observed_faults", []))
    if after and before["boot"] != after["boot"]:
        return {"signature": "master-reset", "confidence": "incomplete", "cause": "unconfirmed"}
    if item.get("collector_failure") or any("capture" in e.lower() or "disconnected" in e.lower() for e in errors):
        return {"signature": "capture-disconnected", "confidence": "incomplete", "cause": "unconfirmed"}
    current = [f for f in frozen if f.get("boot") == before["boot"] and newer(f["sequence"], before["fault"])
               and ((f.get("at", 0) - before["at"]) & 0xffffffff) < 0x80000000]
    fresh = [f for f in current if f.get("op") != 255]
    for fault in fresh:
        samples = fault["samples"][-2:]
        required = {"frame", "core", "ahb", "clock", "irqs", "channels"}
        if len(samples) == 2 and all(required <= s.keys() for s in samples):
            a, b = samples
            running_frames = (b.get("frame", 0) - a.get("frame", 0)) & 0xffff
            enabled = all(s["ahb"] & 0x21 == 0x21 and not s["clock"] & 3 for s in samples)
            rx = all(s["core"] & 0x10 for s in samples)
            no_irq = a.get("irqs") == b.get("irqs") and not a.get("channels") and not b.get("channels")
            owned = any(q & 0x80000000 for q in fault["qtd"])
            powered_connected = fault.get("bus", {}).get("hprt", 0) & 0x1005 == 0x1005
            if running_frames and enabled and no_irq and owned and fault["pending"] and powered_connected:
                return {"signature": "host-no-completion-rx-fifo" if rx else "host-no-completion", "confidence": "observed-symptom",
                        "cause": "unconfirmed", "fault_sequence": fault["sequence"]}
    if fresh:
        return {"signature": "host-control-timeout", "confidence": "observed-fault", "cause": "unconfirmed",
                "fault_sequence": fresh[0]["sequence"]}
    if after:
        manual_only = ((after["fault"] - before["fault"]) & 0xffffffff) == 1 and any(
            f["sequence"] == after["fault"] and f["op"] == 255 for f in current)
        if newer(after["fault"], before["fault"]) and not manual_only:
            return {"signature": "host-fault-without-frozen-evidence", "confidence": "incomplete", "cause": "unconfirmed"}
        if allocation_failure:
            return {"signature": "host-endpoint-allocation-failed", "confidence": "observed-error", "cause": "unconfirmed; allocation stage/heap state required"}
        if any(newer(after["counts"][key], before["counts"][key]) for key in ("dropped", "expired", "overflows")):
            return {"signature": "input-queue-pressure", "confidence": "counter-delta", "cause": "unconfirmed"}
        if newer(after["state"]["recovered"], before["state"]["recovered"]):
            return {"signature": "host-recovery-without-frozen-evidence", "confidence": "incomplete", "cause": "unconfirmed"}
        if (any(after["state"][key] != before["state"][key] for key in ("inputs", "hubs", "controls", "suspended")) or
                after["state"]["state"] != 0):
            return {"signature": "host-input-topology-changed", "confidence": "observed-state", "cause": "unconfirmed"}
    if allocation_failure:
        return {"signature": "host-endpoint-allocation-failed", "confidence": "incomplete", "cause": "unconfirmed; missing status evidence"}
    faults = " ".join(x.get("event", "") for x in item.get("observed_faults", []))
    if any(word in faults for word in ("USB unavailable", "peer restarted", "peer lost", "acknowledgement timeout")):
        return {"signature": "receiver-link-interruption", "confidence": "observed-event", "cause": "unconfirmed; may include host suspend/reboot"}
    progress_failure = any(word in e for e in errors for word in (
        "no USB completion progress", "stopped before the segment ended", "neutral release incomplete"))
    if item.get("partial") and item.get("status") == "FAIL" and progress_failure:
        return {"signature": "source-completion-stall", "confidence": "progress-check", "cause": "unconfirmed"}
    return {"signature": "unclassified-failure" if errors else "no-failure", "confidence": "limited" if after is None else "stream-checks", "cause": "unconfirmed"}
