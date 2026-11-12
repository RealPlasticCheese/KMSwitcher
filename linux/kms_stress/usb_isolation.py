"""Run the dedicated USB-only S3 firmware using existing generator UARTs."""
import argparse
import re
import secrets
import signal
import threading
import time
from pathlib import Path

from .diagnostics import Progress, WorkloadStalled
from .hardware import SerialChannel, read_identity
from .model import Config, PROFILES
from .runner import Cancelled, Evidence, Runner, parse_stats

MODES = {"input": 0, "probe": 1, "led": 2, "both": 3}
HEX = {"boot", "crc", "frame", "port", "core", "mask", "channels", "ahb", "clock",
       "reset", "nptx", "ptx", "host", "interval", "frame-list", "channel-mask", "char", "irq", "size", "dma", "buffer", "transfer", "pipe", "endpoint", "hal", "multi", "list", "urb", "qtd",
       "status", "data", "object", "detail", "schedule-current", "schedule-next", "schedule-mask", "before", "edge-before", "edge-after", "root-status", "root-mask", "root-channels",
       "flags", "state", "first", "second", "current", "device", "endpoints", "ctl", "fifo", "value",
       "endpoint-mask", "in-mask", "device-control"}


def fields(line):
    result = {}
    for token in line.split()[2:]:
        if "=" not in token: raise ValueError("Malformed isolation field")
        key, value = token.split("=", 1)
        if key in result: raise ValueError("Duplicate isolation field")
        if key == "serial":
            if not re.fullmatch(r"[0-9A-F]{12}", value): raise ValueError("Malformed isolation serial")
            result[key] = value; continue
        if not re.fullmatch(r"[0-9a-fA-F]+" if key in HEX else r"\d+", value):
            raise ValueError("Malformed isolation number")
        number = int(value, 16 if key in HEX else 10)
        if number > 0xffffffff: raise ValueError("Isolation counter outside uint32")
        result[key] = number
    return result


def parse_snapshot(lines, token):
    header, devices, retained, ended, workaround = None, {}, [], False, None
    for line in lines:
        if line.startswith("KMSISO BEGIN "):
            item = fields(line)
            if item.get("id") != token: continue
            if header is not None: raise ValueError("Duplicate isolation frame")
            header = item
        if header is None or ended: continue
        retained.append(line)
        if line.startswith("KMSISO DEVICE "):
            item = fields(line)
            required = {"serial", "address", "source", "count", "crc", "sequence-errors", "payload-errors",
                        "usb-errors", "stale", "releases", "pending", "control-pending", "control-start", "controls", "last-report"}
            if not required <= item.keys() or item["serial"] in devices: raise ValueError("Incomplete/duplicate isolation device")
            if item["source"] > 2 or any(item[k] not in (0, 1) for k in ("pending", "control-pending")):
                raise ValueError("Invalid isolation source/flags")
            devices[item["serial"]] = item
        elif line.startswith("KMSISO WORKAROUND "):
            if workaround is not None: raise ValueError("Duplicate workaround evidence")
            workaround = fields(line)
            required_guard = {"version", "enabled", "guard-us", "budget-us", "activations", "waited", "timeouts", "max-wait-us", "crossings"}
            if (not required_guard <= workaround.keys() or workaround["version"] != 1 or
                    workaround["enabled"] > 1 or workaround["guard-us"] != 20 or workaround["budget-us"] != 64):
                raise ValueError("Invalid/incomplete workaround evidence")
        elif line == f"KMSISO END id={token}": ended = True
    required = {"id", "boot", "at", "run", "seed", "profile", "mode", "watching", "fault", "expected", "devices", "frozen"}
    if not ended or header is None or not required <= header.keys() or len(devices) > 2:
        raise ValueError("Incomplete isolation frame")
    if header["mode"] > 3 or header["devices"] > 2 or header["watching"] > 1 or header["frozen"] > 1:
        raise ValueError("Invalid isolation state")
    result = {**header, "sources": devices, "lines": retained}
    if workaround is not None: result["workaround"] = workaround
    trace = parse_driver_trace(retained)
    if trace is not None: result["driver_trace"] = trace
    return result


def parse_driver_trace(lines):
    """Retain structured, bounded first-fault driver evidence; old images remain valid."""
    summary, counts, events, samples, pipes, buffers = None, [], [], [], [], []
    names = {"TRACE": None, "TCOUNT": counts, "TEVENT": events, "TSAMPLE": samples, "TPIPE": pipes, "TBUFFER": buffers}
    for line in lines:
        words = line.split()
        if len(words) < 2 or words[1] not in names: continue
        item = fields(line)
        if words[1] == "TRACE":
            if summary is not None: raise ValueError("Duplicate driver trace")
            summary = item
        else:
            names[words[1]].append(item)
    if summary is None:
        if any((counts, events, samples, pipes, buffers)): raise ValueError("Missing driver trace header")
        return None
    if (summary.get("version") != 1 or summary.get("frozen") != 1 or
            summary.get("events") != len(events) or summary.get("samples") != len(samples) or
            len(events) > 128 or not 1 <= len(samples) <= 6 or len(counts) != 8 or len(pipes) > 48 or len(buffers) > 96
            or summary.get("buffers",len(buffers)) != len(buffers)):
        raise ValueError("Incomplete/unsupported driver trace")
    required = {
        "summary": {"version", "bytes", "events", "overwritten", "samples", "isrs", "last-isr", "frozen"},
        "counts": {"channel", "enqueued", "activated", "irqs", "parsed", "dequeued"},
        "events": {"at", "frame", "kind", "channel", "object", "detail", "qtd", "status"},
        "samples": {"n", "at", "frame", "core", "mask", "channels", "isrs", "last-isr"},
        "pipes": {"sample", "channel", "pipe", "state", "endpoint", "hal", "multi", "pending", "done",
                  "char", "irq", "mask", "size", "dma", "index", "list", "urb", "qtd", "status", "data",
                  "schedule-current", "schedule-next", "schedule-mask"}}
    groups = {"summary": [summary], "counts": counts, "events": events, "samples": samples, "pipes": pipes}
    if any(not required[name] <= item.keys() for name, group in groups.items() for item in group):
        raise ValueError("Missing driver trace fields")
    if {c.get("channel") for c in counts} != set(range(8)):
        raise ValueError("Duplicate/missing driver channel counters")
    if any(e.get("kind", 99) >= 5 or e.get("channel", 99) >= 8 for e in events):
        raise ValueError("Invalid driver trace event")
    if [s.get("n") for s in samples] != list(range(len(samples))):
        raise ValueError("Invalid driver sample order")
    pairs = [(p.get("sample"), p.get("channel")) for p in pipes]
    if (len(set(pairs)) != len(pairs) or
            any(a is None or b is None or a >= len(samples) or b >= 8 for a, b in pairs)):
        raise ValueError("Invalid/duplicate driver pipe sample")
    buffer_pairs = [(b.get("sample"), b.get("channel"), b.get("slot")) for b in buffers]
    if (buffers or "buffers" in summary) and (len(buffers)!=2*len(pipes) or len(set(buffer_pairs))!=len(buffers) or
                    any((a,b) not in pairs or slot not in (0,1) for a,b,slot in buffer_pairs)):
        raise ValueError("Incomplete/duplicate driver buffer samples")
    required_buffer = {"sample","channel","slot","list","urb","flags","state","first","second","index","current","data"}
    if any(not required_buffer <= b.keys() for b in buffers): raise ValueError("Incomplete driver buffer fields")
    return {"summary": summary, "counts": counts, "events": events, "samples": samples, "pipes": pipes, "buffers":buffers}


def verify_streams(snapshot, statistics, generators):
    errors = []
    for source, device in enumerate(generators, 1):
        received = snapshot["sources"].get(device.identity.upper())
        sent = statistics.get(source)
        if not received or not sent:
            errors.append(f"Source {source}: missing completion evidence"); continue
        if received["source"] != source or (received["count"], received["crc"]) != (sent["completed"], sent["crc"]):
            errors.append(f"Source {source}: generator completion and host reception differ")
        if any(received[key] for key in ("sequence-errors", "payload-errors", "usb-errors")):
            errors.append(f"Source {source}: host reported stream/USB errors")
        if not sent["completed"] or sent["submitted"] != sent["completed"] or sent["pending"] or sent["running"]:
            errors.append(f"Source {source}: incomplete generator delivery")
        if not received["releases"]: errors.append(f"Source {source}: neutral release not received")
    return errors


def parse_generator_trace(lines, token, identity, run):
    header, events, samples, ended = None, [], [], False
    for line in lines:
        if line.startswith("KMSGEN TRACE "):
            item = fields(line)
            if item.get("id")!=token: continue
            if header is not None: raise ValueError("Duplicate generator trace")
            header=item
        elif header is not None and not ended:
            if line.startswith("KMSGEN GEVENT "): events.append(fields(line))
            elif line.startswith("KMSGEN GSAMPLE "): samples.append(fields(line))
            elif line==f"KMSGEN TRACEEND id={token}": ended=True
    if (not ended or header is None or header.get("version")!=1 or header.get("frozen")!=1 or
            header.get("serial")!=identity or header.get("run")!=run or
            header.get("events")!=len(events) or header.get("samples")!=len(samples) or
            len(events)>64 or not 1<=len(samples)<=6): raise ValueError("Incomplete/mismatched generator trace")
    required_header={"id","serial","version","run","frozen","events","samples","overwritten","entries",
                     "exits","last-enter","max-isr","arms","accepted","callbacks","resets","suspended","resumed"}
    if not required_header<=header.keys(): raise ValueError("Missing generator trace counters")
    required={"n","at","kind","status","mask","device","endpoints","ctl","irq","size","dma","fifo","ahb","detail","value"}
    for group in [events,samples]:
        if [v.get("n") for v in group]!=list(range(len(group))) or any(not required<=v.keys() or v["kind"]>4 for v in group):
            raise ValueError("Invalid generator trace records")
    return {"summary":header,"events":events,"samples":samples,"lines":lines}


def read_isolation_identity(channel, identity, check, log):
    """Allow bridge-triggered startup; retry only unanswered read-only probes."""
    for attempt in range(4):
        check()
        log("probe", {"line": f"Isolation identity probe {attempt + 1}/4"})
        channel.request("")
        try:
            reply = channel.request("iso info", "KMSISO INFO ", timeout=1.5, service=check)[-1].split()
        except TimeoutError:
            if attempt == 3: raise
            continue
        if (not 8 <= len(reply) <= 10 or len(set(reply[8:])) != len(reply[8:]) or
                not set(reply[8:]) <= {"trace=1", "workaround=1"} or reply[:3] != ["KMSISO", "INFO", "1"] or
                reply[3] != identity.upper() or not re.fullmatch(r"[0-9a-fA-F]{8}", reply[4]) or
                reply[7] not in ("usb=1.5.0", "usb=1.6.0")):
            raise RuntimeError("Master is not the configured USB-isolation firmware/chip")
        return reply


class IsolationRunner:
    def __init__(self, config, seconds=3600, rate=30, mode="input", pattern="stress", notify=print, cancel=None, workaround=True):
        if type(seconds) is not int or not 1 <= seconds <= 86390: raise ValueError("Seconds must be 1..86390")
        if type(rate) is not int or not 1 <= rate <= 1000: raise ValueError("Rate must be 1..1000")
        if mode not in MODES or pattern not in PROFILES: raise ValueError("Unsupported isolation mode/pattern")
        if type(workaround) is not bool: raise ValueError("Workaround must be true or false")
        if len(config.select_master()) != 1 or not config.master.uart: raise ValueError("Select one saved master UART")
        self.devices = [d for d in config.generators if d.enabled]
        if not 1 <= len(self.devices) <= 2: raise ValueError("Enable one or two generators")
        all_devices = [config.master, *self.devices]
        if (any(not re.fullmatch(r"[0-9a-fA-F]{12}", d.identity) or not d.uart for d in all_devices) or
                len({d.identity.upper() for d in all_devices}) != len(all_devices) or
                len({d.uart for d in all_devices}) != len(all_devices)):
            raise ValueError("Isolation chip identities and UART paths must be present and distinct")
        if type(config.seed) is not int or not 0 <= config.seed <= 0xffffffff: raise ValueError("Invalid workload seed")
        if not 1 <= config.max_log_mb <= 16384: raise ValueError("Invalid evidence disk budget")
        self.config, self.seconds, self.rate, self.mode, self.pattern = config, seconds, rate, mode, pattern
        self.notify = notify
        self.control = Runner(config, notify, cancel)
        self.boot = None; self.run_id = 0; self.token = secrets.randbelow(0xfffffffe) + 1
        self.info = None
        self.generator_traces = []
        self.workaround = workaround
        self.workaround_configured = False

    def service(self):
        self.control.service_workload()
        if self.control.master and self.control.master.restarted:
            raise RuntimeError("Isolation master restarted; this run is inconclusive")

    def snapshot(self, fault=False, service=True):
        self.token = self.token % 0xffffffff + 1
        command = "fault" if fault else "status"
        lines = self.control.master.request(f"iso {command} {self.token}", f"KMSISO END id={self.token}",
                    timeout=20, service=self.service if service else None)
        result = parse_snapshot(lines, self.token)
        if getattr(self, "workaround_supported", False) and "workaround" not in result:
            raise ValueError("Master omitted its advertised workaround state")
        if self.workaround_configured and result.get("workaround", {}).get("enabled") != int(self.workaround):
            raise ValueError("Master workaround mode does not match the requested mode")
        if (fault and result["frozen"] and getattr(self, "driver_trace_supported", False)
                and "driver_trace" not in result):
            raise ValueError("Instrumented master did not return its frozen driver trace")
        if self.boot is not None and result["boot"] != self.boot: raise RuntimeError("Isolation master boot changed")
        if self.run_id and result["run"] != self.run_id: raise RuntimeError("Isolation run ID changed")
        self.control.evidence.log("isolation-" + command, result)
        return result

    def prepare(self):
        c = self.control
        c.master = SerialChannel(self.config.master.uart, c.evidence.log)
        reply = read_isolation_identity(c.master, self.config.master.identity, c.check, c.evidence.log)
        self.boot = int(reply[4], 16); self.info = reply
        self.driver_trace_supported = "trace=1" in reply[8:]
        self.workaround_supported = "workaround=1" in reply[8:]
        if self.workaround and not self.workaround_supported:
            raise RuntimeError("Firmware does not support the workaround; flash the updated image or pass --workaround false")
        c.master.restarted = False
        for device in self.devices:
            channel = SerialChannel(device.uart, c.evidence.log); c.generators.append(channel)
            info = read_identity(channel)
            if info.role != "generator" or info.identity != device.identity.upper():
                raise RuntimeError("Generator identity mismatch")
            c.verified_generators.add(channel)
            if info.trace: self.generator_traces.append((channel,device.identity.upper()))
        c.stop_generators()
        initial = self.snapshot()
        if initial["watching"]: raise RuntimeError("Isolation master is already running; stop it before another run")
        if set(initial["sources"]) != {d.identity.upper() for d in self.devices}:
            raise RuntimeError("Native USB inventory does not match the configured generators")
        if self.workaround_supported:
            c.master.request(f"iso workaround {int(self.workaround)}", "KMSISO OK", service=self.service)
            self.workaround_configured = True
            initial = self.snapshot()
        c.evidence.save("isolation-inventory.json", {"info": reply, "snapshot": initial})

    def run(self):
        c = self.control; c.evidence = Evidence(self.config.output, self.config)
        result = {"status": "INCONCLUSIVE", "message": "", "cleanup": [], "segments": [], "hardware_test": True,
                  "workaround": self.workaround,
                  "workload_text": f"USB isolation: {self.mode}; {len(self.devices)} sources; {self.rate}/s each; {self.seconds}s; workaround={str(self.workaround).lower()}",
                  "coverage": {"usb_only": True, "radio": False, "routing": False, "automatic_recovery": False}}
        item = dict(slot=0, rate=self.rate, seconds=self.seconds, profile=self.pattern, mode=self.mode, workaround=self.workaround, errors=[])
        result["segments"].append(item)
        armed, statistics = False, {}
        try:
            self.prepare()
            self.run_id = secrets.randbelow(0xffffffff) + 1
            c.master.request(f"iso arm {self.run_id} {self.config.seed} {PROFILES[self.pattern]} {MODES[self.mode]} {len(self.devices)}", "KMSISO OK", service=self.service)
            armed = True
            for source, channel in enumerate(c.generators, 1):
                channel.request(f"bench config {self.run_id} {source} {self.config.seed} {self.rate} {self.seconds + 10} {PROFILES[self.pattern]}", "KMSGEN CONFIGURED")
            c.segment_open = True
            for source, channel in enumerate(c.generators, 1):
                c.started[source] = time.monotonic()
                channel.request("bench start", f"KMSGEN STARTED {self.run_id}")
            c.master.request("iso watch 1", "KMSISO OK", service=self.service)
            began, next_snapshot, next_stats = time.monotonic(), 0, 0
            progress = Progress(c.started)
            while time.monotonic() - began < self.seconds:
                self.service()
                if time.monotonic() >= next_snapshot:
                    state = self.snapshot()
                    if state["fault"]: raise WorkloadStalled(f"Minimal USB host fault {state['fault']}")
                    self.notify(f"{self.mode}: {time.monotonic() - began:.0f}/{self.seconds}s; " +
                                ", ".join(f"{d['serial']}: {d['count']} reports" for d in state["sources"].values()))
                    next_snapshot = time.monotonic() + 5
                if time.monotonic() >= next_stats:
                    for source, channel in enumerate(c.generators, 1):
                        c.last_stats[source] = parse_stats(channel.request("bench stats", "KMSGEN STATS", timeout=1)[-1], self.run_id, source)
                    for source, stats in c.last_stats.items(): progress.observe(source, stats, time.monotonic())
                    next_stats = time.monotonic() + 1
                c.cancel.wait(0.02)
            c.master.request("iso watch 0", "KMSISO OK", service=self.service)
            c.segment_open = False
            statistics = c.stop_generators(self.run_id)
            c.wait(0.2)
            final = self.snapshot()
            c.evidence.save("isolation-final.json", final)
            item["errors"] = verify_streams(final, statistics, self.devices)
            if final["fault"]: item["errors"].append(f"Minimal USB host fault {final['fault']}")
            item["status"] = "FAIL" if item["errors"] else "PASS"
            guard = final.get("workaround", {})
            if self.workaround and (guard.get("timeouts") or guard.get("crossings")):
                if item["status"] == "PASS": item["status"] = "LIMITED"
                item["errors"].append("Workaround could not protect every activation; inspect timeouts/crossings")
            item.update(c.segment_timing())
            if item["status"] == "PASS" and any(s["completed"] < self.rate * item["generation_seconds"][n] * 0.95 for n, s in statistics.items()):
                item["status"] = "LIMITED"
        except Exception as error:
            item["status"] = "CANCELLED" if isinstance(error, Cancelled) else "FAIL" if isinstance(error, WorkloadStalled) else "INCONCLUSIVE"
            item["errors"].append(str(error))
        finally:
            # Capture before stopping sources or changing watch state. The
            # firmware retains the first fault and never resets the USB bus.
            frozen_generators=[]
            if armed:
                # Freeze every device promptly before the longer host UART dump.
                for channel,identity in self.generator_traces:
                    try:
                        channel.request(f"bench trace freeze {self.token}",f"KMSGEN TRACEFROZEN id={self.token}",timeout=2)
                        frozen_generators.append((channel,identity,self.token))
                    except Exception as error: result["cleanup"].append("Generator trace freeze: "+str(error))
                try:
                    frozen = self.snapshot(fault=True, service=False)
                    c.evidence.save("isolation-fault.json", frozen)
                    if "workaround" in frozen: item["workaround_diagnostics"] = frozen["workaround"]
                    if frozen["fault"] and item["status"] != "CANCELLED": item["status"] = "FAIL"
                except Exception as error: result["cleanup"].append("Fault capture: " + str(error))
                try: c.master.request("iso watch 0", "KMSISO OK", timeout=3)
                except Exception as error: result["cleanup"].append(str(error))
            for channel,identity,token in frozen_generators:
                try:
                    lines=channel.request(f"bench trace dump {token}",f"KMSGEN TRACEEND id={token}",timeout=15)
                    trace=parse_generator_trace(lines,token,identity,self.run_id)
                    c.evidence.save(f"generator-trace-{identity}.json",trace)
                except Exception as error: result["cleanup"].append("Generator trace dump: "+str(error))
            c.segment_open = False
            verified = c.generators
            c.generators = [g for g in verified if g in c.verified_generators]
            if c.generators:
                try: c.stop_generators()
                except Exception as error: result["cleanup"].append(str(error))
            for channel in [c.master, *verified]:
                if channel:
                    try: channel.close()
                    except Exception as error: result["cleanup"].append(str(error))
            item.update(run=self.run_id, generators=statistics or dict(c.last_stats), firmware=self.info)
            if "elapsed_seconds" not in item: item.update(c.segment_timing())
            result["status"] = item["status"]
            if result["cleanup"] and result["status"] in ("PASS", "LIMITED"): result["status"] = "INCONCLUSIVE"
            result["message"] = "; ".join(item["errors"]) or "Captured USB-only input stream verified"
            c.evidence.finish(result)
        self.notify(f"{result['status']}: {result['message']}; {c.evidence.path}")
        return result, c.evidence.path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, default=Path.home() / ".config/kmswitcher-stress/profile.json")
    parser.add_argument("--seconds", type=int, default=3600)
    parser.add_argument("--rate", type=int, default=30)
    parser.add_argument("--mode", choices=MODES, default="input")
    parser.add_argument("--pattern", choices=PROFILES, default="stress")
    parser.add_argument("--workaround", type=str.lower, choices=("true", "false"), default="true",
                        help="Enable the experimental USB frame-boundary guard (default: true); false runs the control")
    parser.add_argument("--output")
    args = parser.parse_args()
    try:
        config = Config.load(args.profile)
        if args.output: config.output = args.output
        cancel = threading.Event()
        signal.signal(signal.SIGINT, lambda *_: cancel.set()); signal.signal(signal.SIGTERM, lambda *_: cancel.set())
        result, _ = IsolationRunner(config, args.seconds, args.rate, args.mode, args.pattern, cancel=cancel,
                                   workaround=args.workaround == "true").run()
        return 0 if result["status"] == "PASS" else 1
    except (OSError, ValueError, RuntimeError) as error: parser.exit(2, str(error) + "\n")


if __name__ == "__main__": raise SystemExit(main())
