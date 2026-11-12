"""One owner for hardware, capture, workloads, cleanup and bounded evidence."""
from dataclasses import asdict
from collections import Counter, deque
from datetime import datetime, timezone
import html
import itertools
import json
from pathlib import Path
import platform
import secrets
import struct
import threading
import time
import traceback
import zlib

from .hardware import Capture, SerialChannel, peers, read_identity
from . import __version__
from .model import (Device, PROFILES, OverlapVerifier, Verifier, handovers, pattern,
                    resilience_seconds, trace_page, verify_overlap)
from .diagnostics import (GeneratorStopError, Progress, WorkloadStalled, checkpoint_boundary,
                          describe_trace, device_fault_errors, overload_pressure, uart_fault)
from .run_progress import RunProgress
from .restarts import RestartCancelled, restart_devices
from .management import inspect_cache, inspect_radio


class Cancelled(Exception):
    pass


class DurationReached(Exception):
    pass


class Evidence:
    def __init__(self, output, config):
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.path = Path(output).expanduser() / (stamp + "-" + secrets.token_hex(3))
        self.path.mkdir(parents=True)
        self.file = (self.path / "capture.jsonl").open("w", encoding="utf-8", newline="\n")
        self.lock = threading.Lock()
        self.size, self.limit = 0, config.max_log_mb * 1024 * 1024
        self.master_port, self.faults = config.master.uart, []
        self.capture_warnings = {}
        self.usb_lines = deque(maxlen=2048)
        self.radio_lines = deque(maxlen=256)
        self.save("profile.json", {"version": 1, **asdict(config)})
        self.save("environment.json", {"app_version": __version__, "python": platform.python_version(),
                  "platform": platform.platform(), "fixture_notes": config.fixture_notes})

    def log(self, kind, data):
        now = time.monotonic_ns()
        text = json.dumps({"host_ns": now, "kind": kind, **data}, separators=(",", ":")) + "\n"
        with self.lock:
            if kind == "capture-warning":
                warning = data["warning"]
                self.capture_warnings[warning] = self.capture_warnings.get(warning, 0) + 1
            if kind == "uart" and data.get("port") == self.master_port and data["line"].startswith(("KMSUSBFAULT ", "KMSUSBSTATE ", "KMSUSBTRACE ")):
                self.usb_lines.append(data["line"])
            if kind == "uart" and data.get("port") == self.master_port:
                if any(marker in data["line"] for marker in ("KMSRADIO ", "KMSCLIPBOARD ", "Radio completion missing", "Radio recovery")):
                    self.radio_lines.append({"host_ns": now, "line": data["line"]})
                event = uart_fault(data["line"])
                if event and self.faults and self.faults[-1]["event"] == event:
                    last = self.faults[-1]
                    last["repeats"] = last.get("repeats", 1) + 1; last["last_host_ns"] = now
                elif event and len(self.faults) < 64:
                    self.faults.append({"host_ns": now, "event": event, "line": data["line"]})
            self.reserve(len(text.encode("utf-8")))
            self.file.write(text)

    def fault_snapshot(self):
        with self.lock: return [dict(item) for item in self.faults]

    def reserve(self, size):
        if self.size + size > self.limit: raise RuntimeError("Evidence disk budget exceeded; run cannot pass")
        self.size += size

    def save(self, name, value, required=False):
        data = (json.dumps(value, indent=2) + "\n").encode("utf-8")
        path = self.path / name
        with self.lock:
            if required:
                path.write_bytes(data)
                return
            previous = path.stat().st_size if path.exists() else 0
            additional = len(data) - previous
            self.reserve(additional)
            try:
                path.write_bytes(data)
            except OSError:
                self.size -= additional
                raise

    def finish(self, result):
        with self.lock:
            self.file.close()
            if self.capture_warnings: result["capture_warnings"] = dict(self.capture_warnings)
        self.save("result.json", result, required=True)
        self.save("radio-diagnostics.json", list(self.radio_lines), required=True)
        rows = []
        for segment in result.get("segments", []):
            detail = "; ".join(segment.get("errors", [])) or segment.get("rate_note", "Verified")
            if segment.get("first_divergence"): detail = segment["first_divergence"] + ": " + detail
            if segment.get("primary_failure"): detail = segment["primary_failure"] + ": " + detail
            achieved = ", ".join(f"G{s}: {rate}" for s, rate in segment.get("actual_reports_per_second", {}).items()) or "—"
            requested = ", ".join(f"G{s}: {rate}" for s, rate in segment.get("requested_reports_per_second", {}).items())
            rows.append("<tr>" + "".join(f"<td>{html.escape(str(x))}</td>" for x in (
                segment["slot"], requested or segment["rate"], achieved, segment.get("seconds", "—"),
                round(segment["elapsed_seconds"], 3) if "elapsed_seconds" in segment else "—",
                segment["profile"], segment["status"], detail)) + "</tr>")
        campaign = ""
        if result.get("campaign"):
            campaign = ("<h2>USB campaign</h2><p>Saved recipe-&lt;case ID&gt;.json files are included in the report ZIP. "
                        "A repeated symptom does not establish its cause. Case results and skipped repeats:</p><pre>" +
                        html.escape(json.dumps(result["campaign"], indent=2)) + "</pre>")
        suite = ""
        if result.get("suite"):
            details = result["suite"]
            suite_rows = []
            for item in details["coverage"].values():
                suite_rows.append("<tr>" + "".join(f"<td>{html.escape(str(v))}</td>" for v in
                    (item["test"], item["slot"], item["rate"], item.get("activity", ""), item.get("sources", []),
                     item["attempts"], item["counts"])) + "</tr>")
            suite = ("<h2>Timed loop coverage</h2>" +
                     f"<p>Completed cycles: {details['completed_cycles']}; stop: {html.escape(details['stop_reason'])}. " +
                     f"Unvisited cases: {len(details['not_run'])}. " +
                     f"Partial cases without a complete attempt: {len(details.get('incomplete', []))}. " +
                     "Final capture and cleanup can finish after the input deadline.</p>" +
                     "<table><tr><th>Test</th><th>Receiver</th><th>Reports/s/source</th><th>Activity</th><th>Sources</th><th>Attempts</th><th>Results</th></tr>" +
                     "".join(suite_rows) + "</table><p>Excluded: " + html.escape("; ".join(details["excluded"])) + "</p>")
        capture_warning = ""
        if result.get("capture_warnings"):
            capture_warning = ("<p>Desktop event guard queue overruns: " +
                str(result["capture_warnings"].get("desktop_event_queue_overrun", 0)) +
                ". Desktop grabs remained active; raw HID reports and device checkpoints still determine the verdict.</p>")
        text = ("<!doctype html><meta charset=utf-8><title>KMSwitcher test report</title>"
                "<style>body{font:16px system-ui;max-width:1100px;margin:40px auto;padding:0 20px}"
                "table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;padding:8px;text-align:left}"
                "pre{white-space:pre-wrap;background:#f5f5f5;padding:15px}</style>"
                f"<h1>KMSwitcher: {html.escape(result['status'])}</h1>"
                f"<p>{html.escape(result.get('message', ''))}</p>"
                f"<p>{html.escape(result.get('workload_text', ''))}</p>"
                + capture_warning +
                "<p>Generator counts and identities are recorded in profile.json. Full evidence: capture.jsonl, "
                "result.json and the trace files. CRC stream checks detect sequence/count changes; they are not "
                "a cryptographic proof. All receivers share this Linux host; Windows/BIOS compatibility is separate.</p>"
                "<table><tr><th>Slave</th><th>Requested reports/s/source</th><th>Achieved reports/s</th>"
                "<th>Planned seconds</th><th>Actual generation seconds</th><th>Profile</th><th>Result</th><th>Detail</th></tr>"
                + "".join(rows) + "</table>" + campaign + suite + "<details><summary>Diagnostic details</summary><pre>" + html.escape(json.dumps(result, indent=2)) + "</pre></details>")
        (self.path / "report.html").write_text(text, encoding="utf-8")
        # Hardware is closed and the verdict is already saved. Ledger generation
        # is offline bookkeeping and must never replace that verdict on failure.
        from .run_ledger import build_record, encoded
        try:
            (self.path / "ledger-record.json").write_text(encoded(build_record(self.path)), encoding="utf-8")
        except (OSError, ValueError, TypeError, KeyError) as error:
            (self.path / "ledger-error.txt").write_text(str(error) + "\n", encoding="utf-8")


def master_traces(master):
    master.request("bench freeze", "KMSBENCH OK")
    first = trace_page(bytes.fromhex(master.request("bench page 0", "KMSBENCH PAGE")[-1].split()[-1]))
    result = {**first, "records": []}
    for index in range(min(first["records_total"], 256)):
        page = first if index == 0 else trace_page(bytes.fromhex(
            master.request(f"bench page {index}", "KMSBENCH PAGE")[-1].split()[-1]))
        if any(page[key] != first[key] for key in ("boot", "run", "records_total")):
            raise RuntimeError("Master restarted or trace changed during download")
        if page["record"]: result["records"].append(page["record"])
    result.pop("record", None)
    result["summary"] = trace_page(bytes.fromhex(master.request("bench page 4294967295", "KMSBENCH PAGE")[-1].split()[-1]))["summary"]
    return result


def parse_stats(line, run, source):
    fields = line.split()
    if len(fields) != 11 or fields[:2] != ["KMSGEN", "STATS"]:
        raise ValueError("Malformed generator statistics")
    values = [int(v, 16 if i == 6 else 10) for i, v in enumerate(fields[2:])]
    if values[0:2] != [run, source]: raise ValueError("Stale/wrong generator statistics")
    if any(v < 0 or v > 0xffffffff for v in values) or values[7] not in (0, 1) or values[8] not in (0, 1):
        raise ValueError("Invalid generator counter/flag")
    if not values[2] >= values[3] >= values[4] or values[5] > values[2]:
        raise ValueError("Inconsistent generator completion counters")
    return dict(zip(("run", "source", "offered", "submitted", "completed", "missed", "crc", "running", "pending"), values))


def serial_hash(identity):
    value = 2166136261
    for byte in identity.encode("utf-16le"): value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def verify_inventory(lines, generators):
    devices, active = {}, set()
    for line in lines:
        fields = line.split()
        if line.startswith("KMSUSB DEVICE "):
            devices[int(fields[2])] = (int(fields[3], 16), int(fields[4], 16), int(fields[5], 16))
        elif line.startswith("KMSUSB INTERFACE ") and fields[8] == "active": active.add(int(fields[2]))
    expected = {(0x303a, 0x4006, serial_hash(d.identity.upper())) for d in generators}
    actual = {devices[address] for address in active if address in devices}
    if actual != expected:
        raise RuntimeError("Master's active USB inputs do not exactly match the configured generators. "
                           "Connect their native USB ports to the master input hub; remove other inputs or select the generators there.")


def diagnose(traces, errors):
    """State evidence bounds explicitly; never infer a vanished trace record."""
    findings = []
    for label, trace in traces.items():
        if "error" in trace:
            findings.append(f"{label}: trace unavailable ({trace['error']})"); continue
        findings.extend(describe_trace(label, trace))
    if errors:
        findings.append("First failed stream: " + errors[0])
        findings.append("Use retained raw/decoded/routed/packet/USB checkpoints to narrow the boundary; missing evidence is inconclusive.")
    return findings


class Runner:
    def __init__(self, config, notify=lambda text: None, cancel=None):
        self.config, self.notify = config, notify
        self.cancel = cancel or threading.Event()
        self.lock = threading.Lock()
        self.verifier, self.active = None, None
        self.leaks, self.segment_open = [], False
        self.capture = self.master = None
        self.generators = []
        self.evidence = None
        self.original_slot = None
        self.master_verified = False
        self.test_mode_entered = False
        self.expected_master_boot = None
        self.expected_usb_boot = None
        self.verified_generators = set()
        self.live_routes = None
        self.live_states = {}
        self.live_seen = {}
        self.direct = None
        self.active_segment = None
        self.started, self.stopped, self.last_stats = {}, {}, {}
        self.next_generator_ping = 0
        self.completed_segments = []
        self.campaign_case = self.campaign_collector = None
        self.campaign_summary = []
        self.timed_deadline = None
        self.suite_summary = None
        self.suite_progress = None
        self.run_progress = None
        self.clipboard_load = None

    def progress(self):
        value = self.run_progress.snapshot() if self.run_progress else None
        if self.suite_progress is not None:
            suite = dict(self.suite_progress)
            if suite.get("running"): suite["remaining_seconds"] = self.remaining_seconds()
            if value is not None:
                suite.update({key: value[key] for key in ("running", "phase", "status", "estimated")})
                if value["phase"] == "finished": suite["remaining_seconds"] = 0
            value = suite
        if value is not None and self.run_progress:
            value["counts"] = dict(Counter(s["status"] for s in self.completed_segments))
        return value

    def planned_seconds(self):
        c = self.config
        if c.preset in ("soak", "all-tests"):
            return c.soak_seconds
        if c.preset == "baseline":
            return len(c.rates) * c.seconds
        if c.preset == "usb-stall":
            from .campaign import cases
            return sum(case.seconds + case.idle_seconds for case in cases(c))
        if c.preset == "mixed-soak":
            return sum(d.enabled for d in c.receivers) * (c.soak_seconds + c.campaign_idle_seconds)
        seconds = sum(args[2] for args in self.schedule())
        if c.preset in ("routing", "full"):
            seconds += self.handover_seconds()
        return seconds

    def handover_seconds(self):
        # Each directed pair visits two targets for 1.5 seconds apiece.
        slots = [d.slot for d in self.config.receivers if d.enabled]
        return 3 * len(handovers(slots))

    def begin_progress_step(self, seconds, test, slot=None, extra=False):
        if self.run_progress:
            self.run_progress.begin_step(seconds, test, slot, extra)

    def end_progress_step(self):
        if self.run_progress:
            self.run_progress.end_step()

    def remaining_seconds(self):
        return max(0, self.timed_deadline - time.monotonic()) if self.timed_deadline is not None else float("inf")

    def duration_reached(self):
        return self.remaining_seconds() <= 0

    def segment_timing(self):
        now = time.monotonic()
        durations = {source: max(0.000001, self.stopped.get(source, now) - began)
                     for source, began in self.started.items()}
        return {"elapsed_seconds": max((self.stopped.get(s, now) for s in self.started), default=now) -
                                   min(self.started.values(), default=now),
                "generation_seconds": durations,
                "actual_reports_per_second": {s: round(item["completed"] / durations[s], 2)
                                               for s, item in self.last_stats.items() if s in durations}}

    def interrupted_segment(self, error, traces):
        if self.active_segment is None: return None
        item = dict(self.active_segment)
        failure = isinstance(error, WorkloadStalled) or (isinstance(error, GeneratorStopError) and error.incomplete)
        item.update(partial=True, status="FAIL" if failure else "INCONCLUSIVE",
                    errors=[str(error)], generators=dict(self.last_stats), **self.segment_timing())
        with self.lock:
            if self.verifier: item["verification_errors"] = list(self.verifier.errors) + list(self.leaks)
        item["observed_faults"] = self.evidence.fault_snapshot()
        item["primary_failure"] = self.evidence.faults[0]["event"] if self.evidence.faults else str(error)
        item["diagnosis"] = diagnose(traces, item["errors"])
        item["shutdown_errors"] = list(getattr(error, "errors", []))
        if self.clipboard_load:
            item["clipboard"] = self.clipboard_load.finish()
            self.clipboard_load.close(); self.clipboard_load = None
        self.active_segment = None
        return item

    def consume(self, slot, interface, data, timestamp):
        with self.lock:
            if not self.segment_open: return
            if self.direct is not None:
                state = self.direct[slot]
                if interface != 0 or len(data) != 55 or data[0] != 1:
                    state["errors"].append("Malformed direct generator report"); return
                payload = data[1:]
                if payload[43:46] != b"KM" + bytes([slot]):
                    state["errors"].append("Direct source identity tag mismatch"); return
                run, sequence = struct.unpack_from("<II", payload, 46)
                if run != state["run"]: return  # release from preceding run
                if sequence == 0: state["released"] = True; return
                if sequence != state["count"] + 1 or payload != pattern(slot, run, sequence, self.config.seed, 1):
                    if len(state["errors"]) < 20: state["errors"].append(f"Direct generator {slot}: sequence/payload mismatch at {sequence}")
                state["count"] += 1; state["crc"] = zlib.crc32(payload, state["crc"])
                return
            if self.live_routes is not None:
                nonneutral = any(data if interface != 2 else data[1:])
                if slot == self.active and nonneutral:
                    self.live_seen[slot] = self.live_seen.get(slot, 0) + 1
                state = self.live_states.setdefault(slot, {0: False, 1: False})
                if interface in (0, 1): state[interface] = any(data if interface == 0 else data[:2])
                expiry = self.live_routes.get(slot, 0)
                if slot != self.active and nonneutral and (time.monotonic() > expiry or not expiry):
                    if len(self.leaks) < 20: self.leaks.append(f"Live handover leaked input to released slave {slot}")
                return
            if slot == self.active:
                self.verifier.feed(interface, data)
            else:
                nonneutral = any(data if interface != 2 else data[1:])
                if nonneutral and len(self.leaks) < 20:
                    self.leaks.append(f"Input leaked to inactive slave {slot}, interface {interface}, at {timestamp}")

    def check(self):
        if self.cancel.is_set(): raise Cancelled("Stopped by operator")
        if self.capture and self.capture.error: raise RuntimeError(str(self.capture.error))
        changed_boot = (self.expected_master_boot and getattr(self.master, "last_boot_id", None) and
                        self.master.last_boot_id != self.expected_master_boot)
        if self.master_verified and (getattr(self.master, "restarted", False) or changed_boot):
            raise RuntimeError("Diagnostic master restarted; it is paused and this run is inconclusive")
        for channel in [self.master, *self.generators]:
            if channel and channel.error: raise RuntimeError(str(channel.error))

    def wait(self, seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.check()
            self.cancel.wait(min(0.05, max(0, until - time.monotonic())))

    def service_workload(self):
        """Keep leases/cancellation live while another UART streams diagnostics."""
        self.check()
        if self.segment_open and self.started and time.monotonic() >= self.next_generator_ping:
            for channel in self.generators: channel.request("bench ping")
            self.next_generator_ping = time.monotonic() + 0.4

    def select_receiver(self, slot):
        try:
            return self.master.request(f"select {slot}", f"KMSROUTE SELECTED {slot}")
        except RuntimeError as error:
            if "selection unavailable" not in str(error): raise
            device = next((d for d in self.config.receivers if d.slot == slot), None)
            name = device.name if device and device.name else f"Receiver {slot}"
            raise RuntimeError(f"{name} (receiver slot {slot}) is unavailable. "
                               f"Try restarting {name}, then start a new run. Details: {error}") from error

    def preflight(self):
        c = self.config
        self.master = SerialChannel(c.master.uart, self.evidence.log)
        info = read_identity(self.master)
        if info.role != "master" or info.identity != c.master.identity.upper(): raise RuntimeError("Master identity does not match the saved profile")
        self.master_verified = True
        self.expected_master_boot = info.boot
        self.master.restarted = False
        self.master.request("about")
        identities, boots, self.original_slot, lines = peers(self.master)
        self.evidence.save("master-inventory.json", {"lines": lines, "boot_ids": boots})
        receivers = [d for d in c.receivers if d.enabled]
        for device in receivers:
            if identities.get(device.slot) != device.identity.upper():
                raise RuntimeError(f"Slave {device.slot}: configured USB identity does not match master's pairing records")
        for device in [d for d in c.generators if d.enabled]:
            channel = SerialChannel(device.uart, self.evidence.log)
            self.generators.append(channel)
            info = read_identity(channel)
            if info.role != "generator" or info.identity != device.identity.upper():
                raise RuntimeError(f"{device.name}: generator identity/protocol mismatch")
            self.verified_generators.add(channel)
            channel.request("bench stop", "KMSGEN STOPPING")
        self.stop_generators()
        self.master.request("test-mode on", "KMSTEST MODE test")
        self.test_mode_entered = True
        verify_inventory(self.master.request("usb-list", "KMSUSB END", timeout=5), [d for d in c.generators if d.enabled])
        self.capture = Capture(receivers, self.consume, self.evidence.log)
        self.capture.open()
        for device in receivers:
            self.capture.feature(device.slot, 1, 1)
            self.capture.feature(device.slot, 2)
            self.capture.page(device.slot, 0)
        # Every configured receiver gets a neutral activation/release before input.
        # No UART is opened with reset lines asserted. Pairings/preferences stay intact.
        for device in receivers:
            self.select_receiver(device.slot)
            self.wait(0.15)
            self.master.request("pause")
            self.wait(0.15)
        self.wait(1.1)
        self.notify(f"Ready: {len(self.generators)} generators, {len(receivers)} captured slaves")

    def baseline(self):
        """Generator-native USB goes directly to Linux; no master/slaves needed."""
        devices = []
        for source, device in enumerate((d for d in self.config.generators if d.enabled), 1):
            channel = SerialChannel(device.uart, self.evidence.log); self.generators.append(channel)
            info = read_identity(channel)
            if info.role != "generator" or info.identity != device.identity.upper():
                raise RuntimeError(f"Generator {source}: identity mismatch")
            self.verified_generators.add(channel)
            channel.request("bench stop", "KMSGEN STOPPING")
            devices.append(Device(device.identity, device.name, slot=source))
        self.capture = Capture(devices, self.consume, self.evidence.log, direct=True); self.capture.open()
        self.stop_generators()
        results = self.completed_segments
        for rate in self.config.rates:
            self.begin_progress_step(self.config.seconds, "baseline", "direct")
            run = secrets.randbelow(0xffffffff) + 1
            self.active_segment = dict(slot="direct", rate=rate, seconds=self.config.seconds, profile="direct baseline", run=run)
            self.started, self.stopped, self.last_stats = {}, {}, {}
            with self.evidence.lock: self.evidence.faults.clear()
            self.notify(f"Direct baseline: {rate} reports/s per generator")
            with self.lock:
                self.direct = {s: {"run": run, "count": 0, "crc": 0, "released": False, "errors": []} for s in range(1, len(devices) + 1)}
                self.segment_open = True
            for source, channel in enumerate(self.generators, 1):
                channel.request(f"bench config {run} {source} {self.config.seed} {rate} {self.config.seconds + 1} 1", f"KMSGEN CONFIGURED {run} {source}")
                self.started[source] = time.monotonic()
                channel.request("bench start", f"KMSGEN STARTED {run}")
            monitor = Progress(self.started)
            until, ping = time.monotonic() + self.config.seconds, 0
            progress = 0
            while time.monotonic() < until:
                self.check()
                if time.monotonic() >= ping:
                    for channel in self.generators: channel.request("bench ping")
                    ping = time.monotonic() + 0.4
                if time.monotonic() >= progress:
                    for source, channel in enumerate(self.generators, 1):
                        self.last_stats[source] = parse_stats(channel.request("bench stats", "KMSGEN STATS")[-1], run, source)
                    for source, item in self.last_stats.items(): monitor.observe(source, item, time.monotonic())
                    progress = time.monotonic() + 1
                self.cancel.wait(0.02)
            stats = self.stop_generators(run); self.wait(0.3)
            errors = []
            with self.lock:
                self.segment_open = False
                for source, state in self.direct.items():
                    errors.extend(state["errors"])
                    if (state["count"], state["crc"], state["released"]) != (stats[source]["completed"], stats[source]["crc"], True) or not state["count"]:
                        errors.append(f"Generator {source}: direct capture/completion/release mismatch")
            status = "FAIL" if errors else "PASS"
            timing = self.segment_timing()
            if status == "PASS" and any(item["completed"] < rate * timing["generation_seconds"][s] * 0.95 for s, item in stats.items()): status = "LIMITED"
            item = {"slot": "direct", "rate": rate, "seconds": self.config.seconds, "profile": "direct baseline", "status": status,
                    "run": run, "errors": errors, "generators": stats,
                    **timing}
            self.active_segment = None
            results.append(item); self.evidence.log("segment", item)
            self.end_progress_step()
            if errors and self.config.stop_on_failure: break
        return results

    def stop_generators(self, run=None):
        stats, errors, waiting = {}, [], set(range(1, len(self.generators) + 1))
        for source, channel in enumerate(self.generators, 1):
            if run is not None: self.stopped.setdefault(source, time.monotonic())
            try: channel.request("bench stop", "KMSGEN STOPPING", timeout=1)
            except Exception as error: errors.append(f"Generator {source}: stop command failed: {error}")
        deadline = time.monotonic() + 3
        while waiting:
            for source in sorted(waiting):
                try:
                    line = self.generators[source - 1].request("bench stats", "KMSGEN STATS", timeout=0.5)[-1]
                    fields = line.split()
                    item = parse_stats(line, int(fields[2]) if run is None else run,
                                       int(fields[3]) if run is None else source)
                    stats[source] = item
                    if run is not None: self.last_stats[source] = item
                    if not item["running"] and not item["pending"]: waiting.remove(source)
                except Exception as error:
                    errors.append(f"Generator {source}: final statistics unavailable: {error}")
                    waiting.remove(source)
            if not waiting or time.monotonic() >= deadline: break
            time.sleep(0.03)
        for source in sorted(waiting):
            item = stats.get(source, {})
            errors.append(f"Generator {source}: neutral release incomplete "
                          f"(running={item.get('running')}, pending={item.get('pending')}, "
                          f"submitted={item.get('submitted')}, completed={item.get('completed')}); "
                          "an earlier USB stall may prevent release submission")
        if errors: raise GeneratorStopError(stats, errors)
        return stats

    def availability_check(self, selected):
        slots = [d.slot for d in self.config.receivers if d.enabled]
        target = next((slot for slot in slots if slot != selected), selected)
        self.notify(f"Testing USB-not-ready selection of slave {target}; generators are stopped")
        self.capture.feature(target, 6, 1)
        try:
            self.wait(1)
            # USB readiness gates delivery, not selection of a reachable peer.
            self.master.request(f"select {target}", f"KMSROUTE SELECTED {target}")
            current = peers(self.master)[2]
            result = {"target": target, "current": current, "passed": current == target,
                      "method": "select reachable peer while USB-ready is false, then restore original route"}
        finally:
            self.capture.feature(target, 6, 0)
            self.wait(1)
            self.master.request(f"select {selected}", f"KMSROUTE SELECTED {selected}")
        _, paired, restored, _ = peers(self.master)
        result["restored"] = restored
        result["passed"] &= restored == selected
        unpaired = next((slot for slot in range(1, 8) if slot not in paired), None)
        if unpaired is not None:
            rejected = False
            try: self.master.request(f"select {unpaired}", f"KMSROUTE SELECTED {unpaired}")
            except RuntimeError as error:
                if "selection unavailable" not in str(error): raise
                rejected = True
            current = peers(self.master)[2]
            result["unpaired_target"] = {"target": unpaired, "rejected": rejected, "current": current,
                                         "passed": rejected and current == selected}
            result["passed"] &= result["unpaired_target"]["passed"]
            if current != selected:
                self.master.request(f"select {selected}", f"KMSROUTE SELECTED {selected}")
        else:
            result["unpaired_target"] = {"skipped": "All seven slots are paired"}
        return result

    def freeze_trace_windows(self):
        """Preserve the detail window without stopping input or summary counts."""
        self.master.request("bench freeze", "KMSBENCH OK")
        for device in self.config.receivers:
            if device.enabled: self.capture.feature(device.slot, 2)

    def collect_traces(self):
        # Freeze every surviving device before the slower UART download, so a
        # failed radio or one missing USB device cannot erase sibling evidence.
        traces = {}
        if self.master and self.master_verified:
            try: self.master.request("bench freeze", "KMSBENCH OK")
            except Exception as error: traces["master"] = {"error": str(error)}
        receivers = [d for d in self.config.receivers if d.enabled] if self.capture and self.config.preset != "baseline" else []
        for d in receivers:
            try: self.capture.feature(d.slot, 2)
            except Exception as error: traces[f"slave_{d.slot}"] = {"error": str(error)}
        if self.master and self.master_verified and "master" not in traces:
            try: traces["master"] = master_traces(self.master)
            except Exception as error: traces["master"] = {"error": str(error)}
        for d in receivers:
            if f"slave_{d.slot}" not in traces:
                try: traces[f"slave_{d.slot}"] = self.capture.traces(d.slot)
                except Exception as error: traces[f"slave_{d.slot}"] = {"error": str(error)}
        return traces

    def segment(self, index, slot, rate, seconds, profile, overload=False, task="", campaign_case=None):
        run = secrets.randbelow(0xffffffff) + 1
        self.active_segment = dict(slot=slot, rate=rate, seconds=seconds, profile=profile, run=run)
        seed = campaign_case.seed if campaign_case else self.config.seed
        source_count = campaign_case.sources if campaign_case else len(self.generators)
        active_sources = set(campaign_case.source_ids) if campaign_case else set(range(1, source_count + 1))
        source_rates = {source: rate for source in active_sources}
        if task == "mixed-soak":
            source_rates = {1: min(self.config.rates), 2: max(self.config.rates)}
            self.active_segment.update(requested_reports_per_second=source_rates,
                                       idle_seconds=self.config.campaign_idle_seconds)
        if campaign_case: self.active_segment.update(case_id=campaign_case.identity, seed=seed, sources=source_count)
        self.started, self.stopped, self.last_stats = {}, {}, {}
        self.verifier = None
        if task == "clipboard":
            from .clipboard import ClipboardTraffic
            source = next(d.slot for d in self.config.receivers if d.enabled and d.slot != slot)
            self.clipboard_load = ClipboardTraffic(self.capture, source, slot, self.evidence.log)
        with self.evidence.lock: self.evidence.faults.clear()
        rates_text = (", ".join(f"G{s}: {requested} reports/s" for s, requested in source_rates.items())
                      if task == "mixed-soak" else f"{rate} reports/s per generator")
        self.notify(f"Segment {index}: slave {slot}, {rates_text}, {profile}, {seconds}s")
        self.select_receiver(slot)
        self.wait(0.25)
        if task == "mixed-soak":
            self.notify(f"Slave {slot}: {self.config.campaign_idle_seconds}s idle before continuous mixed input")
            self.wait(self.config.campaign_idle_seconds)
        radio_before = inspect_radio(self.master, slot) if task == "background" else None
        if task == "clipboard": self.master.request("status")
        if self.duration_reached(): raise DurationReached()
        self.master.request(f"bench arm {run}", f"KMSBENCH ARMED {run}")
        for d in self.config.receivers:
            if d.enabled: self.capture.feature(d.slot, 1, run)
        trace_armed_faults = self.evidence.fault_snapshot()
        if task == "resilience":
            self.master.request("bench drop 5", "KMSBENCH DROP 5")
            self.capture.feature(slot, 5, 7)
        with self.lock:
            sources = sorted(active_sources)
            self.verifier = OverlapVerifier(sources) if profile == "overlap" else Verifier(sources, PROFILES[profile], seed)
            self.leaks, self.active, self.segment_open = [], slot, True
        generation_budget = seconds + (5 if task == "mixed-soak" else 1)
        for source, channel in enumerate(self.generators, 1):
            source_rate = source_rates.get(source, rate)
            channel.request(f"bench config {run} {source} {seed} {source_rate} {generation_budget} {PROFILES[profile]}",
                            f"KMSGEN CONFIGURED {run} {source}")
        # Config changes the run ID, but existing generator firmware only resets
        # counters on start. An idle source can retain a previous run's totals.
        inactive_before = {}
        for source, channel in enumerate(self.generators, 1):
            if source in active_sources: continue
            item = parse_stats(channel.request("bench stats", "KMSGEN STATS")[-1], run, source)
            inactive_before[source] = item
            if item["running"] or item["pending"]:
                raise WorkloadStalled(f"Inactive generator {source} was not neutral before input")
        if campaign_case:
            self.evidence.log("inactive-generator-baseline", {"run": run, "statistics": inactive_before})
        for source, channel in enumerate(self.generators, 1):
            if source not in active_sources: continue
            if not self.started and self.duration_reached(): raise DurationReached()
            self.started[source] = time.monotonic()
            channel.request("bench start", f"KMSGEN STARTED {run}")
        monitor = Progress(self.started)
        began, ping, background, progress, mismatch = time.monotonic(), 0, 0, 0, False
        inspection_checks = 0
        def service_generators():
            nonlocal ping
            if time.monotonic() >= ping:
                for channel in self.generators: channel.request("bench ping")
                ping = time.monotonic() + .4
        windows_frozen = False
        while time.monotonic() - began < seconds and not self.duration_reached():
            self.check()
            if self.clipboard_load: self.clipboard_load.tick()
            if time.monotonic() >= ping:
                for channel in self.generators: channel.request("bench ping")
                ping = time.monotonic() + 0.4
            if self.campaign_collector and self.campaign_collector.poll():
                # A status reply can take 1.2s; keep console load from consuming
                # the generator's two-second heartbeat lease.
                for channel in self.generators: channel.request("bench ping")
            if task == "background" and time.monotonic() >= background:
                # status emits routing/fault diagnostics from the application
                # task. A slow UART must not turn this into an input ACK storm.
                self.master.request("status")
                self.master.request(f"slave-status {slot}")
                inspection = inspect_cache(self.master, service_generators)
                inspection_checks += 1
                self.evidence.log("management-inspection", inspection)
                self.capture.leds(slot, 2)
                background = time.monotonic() + 1
            if campaign_case and time.monotonic() >= background:
                action = {"case_id": campaign_case.identity, "elapsed": time.monotonic() - began}
                if campaign_case.activity in ("led", "background"):
                    mask = 2 if int(time.monotonic() - began) % 2 else 0
                    self.capture.leds(slot, mask); action["led_mask"] = mask
                if campaign_case.activity == "background": self.master.request(f"slave-status {slot}")
                if campaign_case.activity == "console":
                    self.campaign_collector.snapshot()
                    for channel in self.generators: channel.request("bench ping")
                self.evidence.log("campaign-action", action)
                background = time.monotonic() + 1
            if time.monotonic() >= progress:
                completed = []
                for source, channel in enumerate(self.generators, 1):
                    if source not in active_sources: continue
                    item = parse_stats(channel.request("bench stats", "KMSGEN STATS")[-1], run, source)
                    self.last_stats[source] = item
                    completed.append(f"G{source}: {item['completed']} completed, {item['missed']} missed")
                # Sample all sources before failing, so a shared host stall
                # retains both sources' counters rather than blaming source 1.
                for source, item in self.last_stats.items(): monitor.observe(source, item, time.monotonic())
                self.notify(f"Slave {slot}, {time.monotonic() - began:.1f}/{seconds}s — " + "; ".join(completed))
                progress = time.monotonic() + 1
            with self.lock: mismatch = bool(self.verifier.errors or self.leaks)
            # Selection faults still fail this segment, but must not freeze a
            # newly armed trace before it can capture the next fault's cause.
            faults = self.evidence.fault_snapshot()
            if not windows_frozen and (mismatch or faults != trace_armed_faults):
                self.freeze_trace_windows()
                windows_frozen = True
            if (mismatch or faults) and self.config.stop_on_failure and not overload: break
            self.cancel.wait(0.02)
        duration_limited = self.duration_reached() and self.timed_deadline < began + seconds
        # Freeze while the failure window is still present, before lengthy dumps.
        if mismatch and not windows_frozen:
            self.freeze_trace_windows()
        stats = self.stop_generators(run)
        inactive_stats = {s: x for s, x in stats.items() if s not in active_sources}
        stats = {s: x for s, x in stats.items() if s in active_sources}
        self.master.request("bench drop 0", "KMSBENCH DROP 0")
        self.capture.feature(slot, 5, 0)
        self.wait(6 if overload else 0.8)
        self.freeze_trace_windows()
        with self.lock:
            self.segment_open = False
        counts = {s: x["completed"] for s, x in stats.items()}
        if profile == "overlap":
            result = self.verifier.finish(run, seed, counts)
        else:
            result = self.verifier.finish(run, seed, counts, check=self.check)
        result["errors"].extend(self.leaks)
        result.update(self.active_segment, generators=stats)
        if duration_limited:
            result.update(duration_limited=True, partial=True)
        if campaign_case:
            result.update(inactive_generators=inactive_stats, inactive_generator_baselines=inactive_before)
            for source, item in inactive_stats.items():
                if (item["running"] or item["pending"] or any(item[key] != inactive_before[source][key]
                        for key in ("offered", "submitted", "completed", "missed", "crc"))):
                    result["errors"].append(f"Inactive generator {source} produced traffic")
            if campaign_case.activity in ("led", "background"):
                self.capture.leds(slot, 2); self.wait(1)
                for source, channel in enumerate(self.generators, 1):
                    if channel.request("bench leds", "KMSGEN LEDS")[-1].split()[2] != "2":
                        result["errors"].append(f"Generator {source}: keyboard LED feedback failed")
                self.capture.leds(slot, 0)
        result.update(self.segment_timing())
        if self.clipboard_load:
            result["clipboard"] = self.clipboard_load.finish()
            if not result["clipboard"]["passed"] and not duration_limited:
                result["errors"].append("Dongle-only transfer, interrupted receive/cancel or post-transfer input window incomplete")
            self.clipboard_load.close(); self.clipboard_load = None
            self.master.request("status")
        result["observed_faults"] = self.evidence.fault_snapshot()
        if self.evidence.faults: result["primary_failure"] = self.evidence.faults[0]["event"]
        for source, item in stats.items():
            expected_crc = result["comparisons"][f"source_{source}_payload_crc"]
            if (f"{item['crc']:08x}" != expected_crc or item["submitted"] != item["completed"]
                    or (not item["completed"] and not duration_limited)):
                result["errors"].append(f"Source {source}: completion proof failed")
        traces = self.collect_traces()
        for label, trace in traces.items():
            if "error" in trace or trace["run"] != run:
                result["errors"].append(f"{label}: incomplete/reset trace")
        if "summary" in traces["master"] and "summary" in traces[f"slave_{slot}"]:
            m, slave = traces["master"]["summary"], traces[f"slave_{slot}"]["summary"]
            signatures = [(m["reference_keys_count"], m["reference_keys_crc"]), (m["routed_keys_count"], m["routed_keys_crc"]),
                          (slave["completed_keys_count"], slave["completed_keys_crc"]),
                          (result["actual_keyboard"]["count"], int(result["actual_keyboard"]["crc32"], 16))]
            result["keyboard_checkpoints"] = dict(zip(("master_input_reference", "master_routed", "slave_usb_complete", "linux_capture"), signatures))
            if len(set(signatures)) != 1: result["errors"].append("Merged keyboard stream diverged between input/routing/slave USB/Linux checkpoints")
            boundary = checkpoint_boundary(signatures, traces["master"], self.evidence.faults)
            if boundary: result["first_divergence"] = boundary
            if signatures[0] == signatures[1] == signatures[2] and signatures[2] != signatures[3]:
                result["collector_failure"] = True
            for source, item in stats.items():
                if (m[f"source{source}_count"], m[f"source{source}_crc"]) != (item["completed"], item["crc"]):
                    result["errors"].append(f"Source {source}: generator-to-master USB stream mismatch")
                    result["first_divergence"] = f"Generator {source} USB completion → master USB reception"
        if profile == "overlap": verify_overlap(result, traces["master"], counts)
        if task == "background":
            result["management_inspection"] = dict(checks=inspection_checks, update_guard_clear=inspection_checks > 0)
            radio_after = inspect_radio(self.master, slot)
            result["receiver_radio"] = dict(before=radio_before, after=radio_after)
            if radio_before.get("supported") and radio_after.get("supported"):
                if radio_before["boot"] != radio_after["boot"]:
                    result["errors"].append("Receiver restarted during Background input verification")
                else:
                    failed = (radio_after["tx_failed"] - radio_before["tx_failed"]) & 0xffffffff
                    if failed: result["errors"].append(f"Receiver {slot}: {failed} failed radio completion(s) during input")
            for source, channel in enumerate(self.generators, 1):
                if channel.request("bench leds", "KMSGEN LEDS")[-1].split()[2] != "2":
                    result["errors"].append(f"Generator {source}: selected-host keyboard LED feedback failed")
        if task == "resilience":
            result["injections"] = {
                role: traces[label].get("summary", {}).get("injections")
                for role, label in (("input", "master"), ("ack", f"slave_{slot}"))
            }
            if not duration_limited:
                result["receiver_readiness"] = self.availability_check(slot)
                if not result["receiver_readiness"]["passed"]:
                    result["errors"].append("USB-readiness selection, route restoration or unpaired-target rejection failed")
        self.evidence.save(f"trace-{index:05d}.json", traces)
        fault_errors = device_fault_errors(result["observed_faults"], traces)
        result["errors"].extend(fault_errors)
        if fault_errors: result.setdefault("primary_failure", fault_errors[0].removeprefix("Device fault: "))
        result["diagnosis"] = diagnose(traces, result["errors"])
        result["status"] = "FAIL" if result["errors"] else "PASS"
        if (result.get("collector_failure") or any("incomplete/reset trace" in e for e in result["errors"]) or
                (result.get("overlap_reference_incomplete") and not fault_errors)):
            result["status"] = "INCONCLUSIVE"
        if result["status"] == "PASS" and any(
                item["completed"] < source_rates[s] * result["generation_seconds"][s] * 0.95 for s, item in stats.items()):
            result["status"] = "LIMITED"
            result["rate_note"] = "Streams verified at achieved rate; requested load was not sustained by the generators/USB polling."
        # Overload is characterization; unrelated device faults still fail.
        evidence_problem = any("incomplete/reset trace" in e or "completion proof" in e for e in result["errors"])
        held = any("still held" in e for e in result["errors"])
        if (overload and result["status"] == "FAIL" and not duration_limited and
                not self.leaks and not evidence_problem and not held and
                overload_pressure(result["observed_faults"], traces)):
            result["status"] = "OVERLOAD OBSERVED"
            result["requires_recovery_check"] = True
        incomplete = []
        if duration_limited:
            incomplete.append("Run deadline ended input before the planned case duration")
        if task == "resilience" and not all(result["injections"].values()):
            incomplete.append("The master input drop and receiver ACK drop were not both observed")
        if (profile == "overlap" and source_count == 2
                and result.get("overlap_evidence") != {"key": True, "button": True}):
            incomplete.append("Both generators did not hold the same key and button concurrently")
        if incomplete:
            result["errors"].extend(incomplete)
            if result["status"] in ("PASS", "LIMITED"):
                result["status"] = "INCONCLUSIVE"
            result["partial"] = True
            result["diagnosis"] = diagnose(traces, result["errors"])
        self.evidence.log("segment", result)
        self.active_segment = None
        self.notify(f"{result['status']}: slave {slot}")
        return result

    def live_handover(self, index):
        """Continuous input across every directed pair; checks isolation/release.

        Individual sample delivery at a switch boundary is intentionally not a
        lossless requirement: switching discards the old route's backlog.
        """
        slots = [d.slot for d in self.config.receivers if d.enabled]
        targets = [s for pair in handovers(slots) for s in pair]
        if not targets: return None
        run = secrets.randbelow(0xffffffff) + 1
        self.active_segment = dict(slot="all", rate=min(self.config.rates[0], 50), seconds=0,
                                   profile="continuous handover safety", run=run)
        self.started, self.stopped, self.last_stats = {}, {}, {}
        with self.evidence.lock: self.evidence.faults.clear()
        self.master.request(f"bench arm {run}", f"KMSBENCH ARMED {run}")
        for d in self.config.receivers:
            if d.enabled: self.capture.feature(d.slot, 1, run)
        duration = min(86400, len(targets) * 2 + 10)
        self.master.request(f"select {targets[0]}", f"KMSROUTE SELECTED {targets[0]}")
        self.wait(0.3)
        with self.lock:
            self.live_routes, self.live_states, self.live_seen, self.leaks = {}, {}, {}, []
            self.active, self.segment_open = targets[0], True
        for source, channel in enumerate(self.generators, 1):
            if not self.started and self.duration_reached(): raise DurationReached()
            channel.request(f"bench config {run} {source} {self.config.seed} {min(self.config.rates[0], 50)} {duration} 0",
                            f"KMSGEN CONFIGURED {run} {source}")
            self.started[source] = time.monotonic()
            channel.request("bench start", f"KMSGEN STARTED {run}")
        transitions, previous = set(), targets[0]
        windows_frozen = False
        try:
            for slot in targets:
                if self.duration_reached(): break
                self.notify(f"Live handover: {previous} → {slot}")
                # Serialize the route command with capture classification: reports
                # arriving while the command is acknowledged belong to this boundary.
                with self.lock:
                    seen_before = self.live_seen.get(slot, 0)
                    self.live_routes[previous] = time.monotonic() + 1.25
                    self.select_receiver(slot)
                    self.active = slot
                until, ping = time.monotonic() + 1.5, 0
                while time.monotonic() < until and not self.duration_reached():
                    self.check()
                    if time.monotonic() >= ping:
                        for channel in self.generators: channel.request("bench ping")
                        ping = time.monotonic() + 0.4
                    with self.lock:
                        for inactive, state in self.live_states.items():
                            if inactive != self.active and any(state.values()) and time.monotonic() > self.live_routes.get(inactive, 0):
                                if len(self.leaks) < 20: self.leaks.append(f"Slave {inactive}: held input not released after handover")
                        mismatch = bool(self.leaks)
                    fault = bool(self.evidence.fault_snapshot())
                    if (mismatch or fault) and not windows_frozen:
                        self.freeze_trace_windows()
                        windows_frozen = True
                    if (mismatch or fault) and self.config.stop_on_failure:
                        break
                    self.cancel.wait(0.02)
                if self.duration_reached():
                    break
                with self.lock:
                    received = self.live_seen.get(slot, 0) > seen_before
                    if not received:
                        self.leaks.append(f"Slave {slot}: no active input observed after handover")
                if self.config.stop_on_failure and (self.leaks or self.evidence.fault_snapshot()):
                    break
                if previous != slot and received:
                    transitions.add((previous, slot))
                previous = slot
            deadline_limited = self.duration_reached()
            stats = self.stop_generators(run)
            self.master.request("pause")
            self.wait(1.2)
            with self.lock:
                errors = list(self.leaks)
                for slot, state in self.live_states.items():
                    if any(state.values()): errors.append(f"Slave {slot}: held input after live test")
            incomplete = not set(handovers(slots)).issubset(transitions)
            if incomplete and not deadline_limited: errors.append("Not all directed handovers completed")
            for source, item in stats.items():
                crc = 0
                for sequence in range(1, item["completed"] + 1):
                    crc = zlib.crc32(pattern(source, run, sequence, self.config.seed, 0), crc)
                if not item["completed"] or item["submitted"] != item["completed"] or item["crc"] != crc:
                    errors.append(f"Source {source}: live handover completion proof failed")
            result = {"slot": "all", "rate": min(self.config.rates[0], 50), "profile": "continuous handover safety",
                      "run": run, "errors": errors, "status": "FAIL" if errors else "PASS",
                      "generators": stats, "active_input_reports": dict(self.live_seen),
                      "transitions": sorted(transitions), "checks": "destination isolation and releases; switch-boundary backlog may be discarded"}
            traces = self.collect_traces()
            result["observed_faults"] = self.evidence.fault_snapshot()
            fault_errors = device_fault_errors(result["observed_faults"],
                                               {label: trace for label, trace in traces.items() if trace.get("run") == run})
            if fault_errors:
                result["errors"].extend(fault_errors)
                result.update(status="FAIL", primary_failure=fault_errors[0].removeprefix("Device fault: "))
            if incomplete:
                result["partial"] = True
                if deadline_limited:
                    result["duration_limited"] = True
                if result["status"] == "PASS":
                    result["status"] = "INCONCLUSIVE"
                    result["errors"].append("Run deadline ended input before all directed handovers were verified")
            if any("error" in trace or trace.get("run") != run for trace in traces.values()):
                result["errors"].append("Incomplete/reset handover trace")
                if result["status"] == "PASS": result["status"] = "INCONCLUSIVE"
            self.evidence.save(f"trace-{index:05d}.json", traces)
            self.evidence.log("segment", result)
            self.active_segment = None
            return result
        finally:
            with self.lock: self.segment_open = False; self.live_routes = None

    def schedule(self):
        c = self.config
        slots = [d.slot for d in c.receivers if d.enabled]
        if c.preset in ("overlap",):
            for slot in slots: yield slot, 2, 2, "overlap", False
        elif c.preset == "resilience":
            rate = min(c.rates[0], 25)
            for slot in slots: yield slot, rate, resilience_seconds(c.seconds, rate), "everyday", False, "resilience"
        elif c.preset == "background":
            for slot in slots: yield slot, c.rates[0], max(c.seconds, 3), "everyday", False, "background"
        elif c.preset == "clipboard":
            for slot in slots:
                yield slot, c.rates[0], max(c.seconds, 15), "everyday", False, "clipboard"
                yield slot, c.rates[0], max(c.seconds, 3), "everyday", False
        elif c.preset == "media":
            for slot in slots: yield slot, c.rates[0], c.seconds, "media", False
        elif c.preset == "routing":
            targets = [s for pair in handovers(slots) for s in pair] or slots
            for slot in targets: yield slot, c.rates[0], c.seconds, "everyday", False
        elif c.preset == "limit":
            for rate, slot in itertools.product(sorted(set(c.rates)), slots): yield slot, rate, c.seconds, "stress", False
        elif c.preset == "overload":
            for slot in slots:
                yield slot, max(c.rates), c.seconds, "stress", True
                yield slot, min(c.rates), max(c.seconds, 3), "everyday", False
        elif c.preset == "soak":
            until = time.monotonic() + c.soak_seconds
            if self.run_progress: self.run_progress.start_timed(until)
            for slot in itertools.cycle(slots):
                if time.monotonic() >= until: break
                yield slot, c.rates[len(c.rates) // 2], min(c.seconds, max(1, int(until - time.monotonic()))), "stress", False
        elif c.preset == "mixed-soak":
            for slot in slots:
                yield slot, max(c.rates), c.soak_seconds, "mixed", False, "mixed-soak"
        elif c.preset == "full":
            for slot in slots:
                yield slot, c.rates[0], c.seconds, "everyday", False
                yield slot, 2, 2, "overlap", False
                resilience_rate = min(c.rates[0], 25)
                yield slot, resilience_rate, resilience_seconds(c.seconds, resilience_rate), "everyday", False, "resilience"
                yield slot, c.rates[0], max(c.seconds, 3), "everyday", False, "background"
                yield slot, c.rates[0], c.seconds, "media", False
            for rate, slot in itertools.product(sorted(set(c.rates)), slots): yield slot, rate, c.seconds, "stress", False
        else:
            profile = "pointer" if c.preset == "pointer" else "everyday"
            for slot in slots: yield slot, c.rates[0], c.seconds, profile, False

    def run(self):
        self.config.validate()
        self.run_progress = RunProgress(self.planned_seconds(), self.config.preset,
                                        timed=self.config.preset in ("soak", "all-tests"))
        self.evidence = Evidence(self.config.output, self.config)
        self.completed_segments = []
        self.campaign_summary = []
        self.campaign_case = self.campaign_collector = None
        self.expected_usb_boot = None
        result = {"status": "INCONCLUSIVE", "segments": self.completed_segments, "message": "", "cleanup": [],
                  "workload": self.config.workload(),
                  "workload_text": self.config.workload_text(),
                  "coverage": {"live_handover": False, "physical_power_cycles": False,
                               "system_key_generation": False, "overlap_pattern": False, "shared_key_overlap": False,
                               "windows_bios": False}, "hardware_test": True}
        try:
            if self.config.restart_before_run:
                self.notify("Restarting enabled devices before the test...")
                restarted = restart_devices(self.config, self.notify, self.cancel, self.evidence.log, enabled_only=True)
                self.evidence.save("device-restarts.json", restarted, required=True)
            self.notify("Workload: " + self.config.workload_text())
            if self.config.preset == "baseline": result["segments"] = self.baseline()
            else:
                self.preflight()
                if self.config.preset == "usb-stall":
                    from .campaign import run
                    run(self)
                    result["coverage"]["usb_campaign"] = True
                elif self.config.preset == "all-tests":
                    from .suite import run
                    run(self)
                    for key, name in (("media_generation", "media"),
                                      ("live_handover", "live-handover"), ("usb_campaign", "usb-stall")):
                        result["coverage"][key] = any(s.get("test") == name and s["status"] == "PASS"
                                                       for s in self.completed_segments)
                else:
                    for index, args in enumerate(self.schedule(), 1):
                        seconds = args[2] + (self.config.campaign_idle_seconds if self.config.preset == "mixed-soak" else 0)
                        self.begin_progress_step(seconds, args[5] if len(args) > 5 else args[3], args[0])
                        item = self.segment(index, *args)
                        result["segments"].append(item)
                        self.end_progress_step()
                        if args[3] == "media": result["coverage"]["media_generation"] = True
                        if item["status"] in ("FAIL", "INCONCLUSIVE") and self.config.stop_on_failure: break
            overlap = [s for s in result["segments"] if s.get("profile") == "overlap" and s["status"] == "PASS"]
            result["coverage"]["overlap_pattern"] = bool(overlap)
            # One source exercises the pattern, but cannot establish that two
            # sources sharing a held key/button merge and release correctly.
            result["coverage"]["shared_key_overlap"] = any(
                len(s.get("generators", {})) >= 2 and s.get("overlap_evidence") == {"key": True, "button": True}
                for s in overlap
            )
            if self.config.preset in ("routing", "full") and not any(x["status"] in ("FAIL", "INCONCLUSIVE") for x in result["segments"]):
                self.begin_progress_step(self.handover_seconds(), "live handover")
                item = self.live_handover(len(result["segments"]) + 1)
                self.end_progress_step()
                if item:
                    result["segments"].append(item)
                    result["coverage"]["live_handover"] = item["status"] == "PASS"
            result["status"] = "FAIL" if any(x["status"] == "FAIL" for x in result["segments"]) else "PASS"
            if result["status"] == "PASS" and any(x["status"] == "INCONCLUSIVE" for x in result["segments"]): result["status"] = "INCONCLUSIVE"
            if result["status"] == "PASS" and any(x["status"] == "LIMITED" for x in result["segments"]): result["status"] = "LIMITED"
            if not result["segments"]: result["status"] = "INCONCLUSIVE"
            result["message"] = "Completed captured-device tests. Coverage exclusions are explicit below."
            if self.suite_summary and (self.suite_summary["not_run"] or self.suite_summary.get("incomplete")):
                if result["status"] != "FAIL":
                    result["status"] = "INCONCLUSIVE"
                result["message"] = "The timed run ended before every planned case had a complete attempt"
            failed = next((x for x in result["segments"] if x["status"] in ("FAIL", "INCONCLUSIVE")), None)
            if failed:
                result["message"] = failed.get("primary_failure") or next(iter(failed.get("errors", [])), "Incomplete evidence")
        except Exception as error:
            cancelled = isinstance(error, (Cancelled, RestartCancelled))
            result["status"], result["message"] = "CANCELLED" if cancelled else "INCONCLUSIVE", str(error)
            self.evidence.save("exception.json", {"error": str(error), "traceback": traceback.format_exc()}, required=True)
            # Best-effort evidence on capture/serial loss, before closing any
            # surviving USB connections. Stop sources before lengthy downloads.
            shutdown_errors = list(getattr(error, "errors", []))
            for source, channel in enumerate(self.generators, 1):
                if channel not in self.verified_generators: continue
                self.stopped.setdefault(source, time.monotonic())
                try: channel.request("bench stop")
                except Exception as stop_error: shutdown_errors.append(str(stop_error))
            if self.active_segment and isinstance(error, WorkloadStalled):
                try: self.stop_generators(self.active_segment["run"])
                except GeneratorStopError as stop_error: shutdown_errors.extend(stop_error.errors)
            traces = self.collect_traces()
            self.evidence.save("interrupted-traces.json", traces, required=True)
            partial = self.interrupted_segment(error, traces)
            if partial:
                partial["shutdown_errors"] = shutdown_errors
                if cancelled: partial["status"] = "CANCELLED"
                result["segments"].append(partial)
                result["status"] = partial["status"]
                primary = partial["primary_failure"]
                result["message"] = primary if primary == str(error) else primary + ": " + str(error)
        finally:
            self.run_progress.cleanup()
            if self.suite_summary is not None: result["suite"] = self.suite_summary
            if self.config.preset == "usb-stall": result["campaign"] = self.campaign_summary
            with self.lock: self.segment_open = False
            if self.clipboard_load:
                try: self.clipboard_load.close()
                except Exception as error: result["cleanup"].append(str(error))
                self.clipboard_load = None
            for channel in self.generators:
                if channel not in self.verified_generators: continue
                try: channel.request("bench stop", "KMSGEN STOPPING")
                except Exception as error: result["cleanup"].append(str(error))
            if self.master and self.master_verified:
                try:
                    self.master.request("pause")
                    self.master.request("bench off", "KMSBENCH OK")
                except Exception as error: result["cleanup"].append(str(error))
            # Keep exclusive input grabs until stop/release traffic and leases
            # have had time to settle, including an unresponsive generator's
            # two-second heartbeat timeout. The UI remains responsive.
            if self.capture: time.sleep(3.2)
            if self.test_mode_entered:
                try:
                    self.stop_generators()
                    if self.capture and self.capture.error: raise RuntimeError(str(self.capture.error))
                except Exception as error: result["cleanup"].append(str(error))
            if self.capture:
                if self.config.preset != "baseline":
                    for d in self.config.receivers:
                        if d.enabled:
                            if self.config.preset == "usb-stall":
                                try: self.capture.leds(d.slot, 0)
                                except Exception as error: result["cleanup"].append(str(error))
                            try: self.capture.feature(d.slot, 3)
                            except Exception as error: result["cleanup"].append(str(error))
                try: self.capture.close()
                except Exception as error: result["cleanup"].append(str(error))
            # Leave a failed/interrupted session guarded across restarts. Only
            # return to everyday startup after all source/capture cleanup succeeds.
            if self.test_mode_entered and not result["cleanup"] and result["status"] in ("PASS", "LIMITED", "CANCELLED"):
                try:
                    self.master.request("test-mode off", "KMSTEST MODE everyday")
                except Exception as error: result["cleanup"].append(str(error))
            elif self.test_mode_entered:
                result["cleanup"].append("Test mode retained; stop/disconnect generators, then use test-mode off to restore everyday startup")
            for channel in self.generators:
                try: channel.close()
                except Exception as error: result["cleanup"].append(str(error))
            if self.master:
                try: self.master.close()
                except Exception as error: result["cleanup"].append(str(error))
            if result["cleanup"] and result["status"] == "PASS": result["status"] = "INCONCLUSIVE"
            self.evidence.finish(result)
            self.run_progress.finish(result["status"])
        self.notify(f"{result['status']}: {result['message']} Report: {self.evidence.path / 'report.html'}")
        return result, self.evidence.path


def demo(config):
    """Exercise the real verifier/report writer with synthetic receiver reports."""
    evidence = Evidence(config.output, config)
    segments = []
    slots = [d.slot for d in config.receivers if d.enabled] or [1, 2, 3, 4, 5, 6]
    sources = list(range(1, len([d for d in config.generators if d.enabled]) + 1)) or [1, 2]
    for slot in slots:
        verifier, states = Verifier(sources, 1), {s: pattern(s, 1, 0, config.seed, 1) for s in sources}
        for sequence, source in itertools.product(range(1, 101), sources):
            data = pattern(source, 1, sequence, config.seed, 1); states[source] = data
            keys = modifiers = buttons = 0
            for state in states.values():
                modifiers |= state[0]; keys |= int.from_bytes(state[1:33], "little")
                buttons |= int.from_bytes(state[33:35], "little")
            keyboard = bytes([modifiers, 0]) + keys.to_bytes(32, "little")
            mouse = struct.pack("<H", buttons) + data[35:41]
            verifier.feed(0, keyboard); verifier.feed(1, mouse)
            evidence.log("demo_hid", {"slot": slot, "keyboard": keyboard.hex(), "mouse": mouse.hex()})
        verifier.feed(0, bytes(34)); verifier.feed(1, bytes(8))
        item = verifier.finish(1, config.seed, {s: 100 for s in sources})
        item.update(slot=slot, rate=100, profile="stress", status="FAIL" if item["errors"] else "PASS")
        segments.append(item)
    result = {"status": "DEMO PASS" if all(s["status"] == "PASS" for s in segments) else "DEMO FAIL",
              "message": "Simulated devices only; this does not validate hardware.", "segments": segments, "hardware_test": False}
    evidence.finish(result)
    return result, evidence.path
