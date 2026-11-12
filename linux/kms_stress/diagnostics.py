"""Workload progress and evidence-based failure descriptions."""
import re


EVENTS = ("boot", "acknowledgement timeout", "peer lost", "peer restarted", "USB unavailable",
          "transmit queue overflow", "receive queue overflow", "USB stall", "lease expired",
          "input disconnected", "input error", "input overflow", "LED failed", "LED pending",
          "replay dropped", "connection recovered", "selection unavailable", "selected")
RECOVERY_EVENTS = {15, 17}
NONFAULT_NAMES = {"connection recovered", "computer selected", "selected", "USB host reconnect", "USB host recovered"}
QUEUE_PRESSURE = {"transmit queue overflow", "receive queue overflow", "input overflow"}


class WorkloadStalled(RuntimeError):
    """A configured, running workload stopped completing USB reports."""


class GeneratorStopError(RuntimeError):
    def __init__(self, stats, errors):
        self.stats, self.errors = stats, errors
        self.incomplete = any(item["running"] or item["pending"] for item in stats.values())
        super().__init__("; ".join(errors))


class Progress:
    def __init__(self, started, timeout=3.0):
        self.previous = {source: (0, when) for source, when in started.items()}
        self.timeout = timeout

    def observe(self, source, stats, now):
        completed, changed = self.previous[source]
        if stats["completed"] != completed:
            changed = now
        self.previous[source] = stats["completed"], changed
        if not stats["running"]:
            raise WorkloadStalled(f"Generator {source} stopped before the segment ended")
        if now - changed >= self.timeout:
            raise WorkloadStalled(
                f"Generator {source}: no USB completion progress for {now - changed:.1f}s "
                f"({stats['submitted']} submitted, {stats['completed']} completed, "
                f"pending={stats['pending']}, {stats['missed']} missed)")


def uart_fault(line):
    if "link:" in line and any(marker in line for marker in (
            "Radio completion missing", "Radio recovery failed", "Radio recovery budget exhausted")):
        return "Radio transmit recovery: " + line.split("link:", 1)[1].strip()
    if "ESP_ERR_NO_MEM" in line and any(prefix in line for prefix in (
            "USBH: EP Alloc error:", "USB HOST: EP allocation error ", "USB HOST: Claiming interface error:")):
        return "USB endpoint allocation failed"
    match = re.search(r"diagnostics: Slot \d+: (.*?) \(detail=", line)
    if match and match[1] not in NONFAULT_NAMES:
        return match[1]
    if "usb-recovery:" in line and any(word in line for word in ("Unhealthy", "failed", "deadline", "stopped")):
        return "USB recovery: " + line.split("usb-recovery:", 1)[1].strip()
    return None


def trace_events(trace):
    return [record for record in trace.get("records", []) if record["stage"] == 12]


def fault_records(trace):
    return [record for record in trace_events(trace) if record["a"] not in RECOVERY_EVENTS]


def device_fault_errors(observed, traces):
    """Delivery success cannot erase a device fault or its required recovery."""
    errors = list(dict.fromkeys("Device fault: " + item["event"] for item in observed))
    for label, trace in traces.items():
        for record in fault_records(trace):
            code = record["a"]
            name = EVENTS[code] if 0 <= code < len(EVENTS) else f"event {code}"
            if "Device fault: " + name not in errors:
                message = f"Device fault: {label}: {name}"
                if message not in errors: errors.append(message)
    return errors


def overload_pressure(observed, traces):
    """Queue saturation can explain delivery faults during a capacity test."""
    events = set()
    for trace in traces.values():
        for record in fault_records(trace):
            code = record["a"]
            events.add(EVENTS[code] if 0 <= code < len(EVENTS) else f"event {code}")
    if not events & QUEUE_PRESSURE:
        return False
    events.update(item["event"] for item in observed)
    return events <= QUEUE_PRESSURE | {"acknowledgement timeout", "lease expired"}


def describe_trace(label, trace):
    if "error" in trace:
        return [f"{label}: trace unavailable ({trace['error']})"]
    events = trace_events(trace)
    faults = fault_records(trace)
    findings = []
    for record in faults:
        code = record["a"]
        name = EVENTS[code] if 0 <= code < len(EVENTS) else f"event {code}"
        findings.append(f"{label}: {name} (detail={record['c']}, device_us={record['micros']})")
    recovered = sum(r["a"] == 15 for r in events)
    if recovered:
        findings.append(f"{label}: {recovered} recovery notifications in retained window")
    if trace.get("overwritten"):
        findings.append(f"{label}: rolling window only; {trace['overwritten']} older records overwritten")
    return findings


def checkpoint_boundary(signatures, master_trace, observed_faults):
    if signatures[0] != signatures[1]:
        # Fault handling deliberately changes the held-state stream. A CRC
        # boundary identifies an observation, not proof of a parser defect.
        if observed_faults or trace_events(master_trace) or master_trace.get("summary", {}).get("faults"):
            return "Master input/routed streams differ during fault/recovery; initiating cause requires fault evidence"
        return "Master input/routed stream mismatch; cause unconfirmed"
    if signatures[1] != signatures[2]:
        return "Master routed stream / receiver USB completion"
    if signatures[2] != signatures[3]:
        return "Linux HID capture path"
    return None
