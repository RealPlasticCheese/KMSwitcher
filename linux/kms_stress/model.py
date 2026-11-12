"""Independent workload oracle. No firmware code is loaded by this module."""
from dataclasses import asdict, dataclass, field
import json
import math
from pathlib import Path
import re
import struct
import zlib

MIXED_PROFILE = 5
MAX_MIXED_SOAK_SECONDS = 86390
PROFILES = {"everyday": 0, "stress": 1, "pointer": 2, "overlap": 3, "media": 4, "mixed": MIXED_PROFILE}
PRESETS = ("baseline", "quick", "everyday", "routing", "overlap", "resilience", "background", "clipboard",
           "media", "limit", "overload", "soak", "mixed-soak", "pointer", "full", "usb-stall", "all-tests")


def resilience_seconds(seconds, rate):
    # Allow multiple opportunities for both the fifth input and seventh ACK drops.
    return max(seconds, 3, math.ceil(14 / max(1, rate)) + 1)


@dataclass
class Device:
    identity: str
    name: str = ""
    uart: str = ""
    slot: int = 0
    enabled: bool = True


@dataclass
class Config:
    master: Device = field(default_factory=lambda: Device("", "Master"))
    generators: list[Device] = field(default_factory=list)
    receivers: list[Device] = field(default_factory=list)
    preset: str = "quick"
    rates: list[int] = field(default_factory=lambda: [10, 25, 50, 100, 250, 500, 1000])
    seconds: int = 10
    seed: int = 1
    soak_seconds: int = 28800
    output: str = field(default_factory=lambda: str(Path.home() / ".local/share/kmswitcher-stress/results"))
    max_log_mb: int = 1024
    stop_on_failure: bool = True
    campaign_repeats: int = 3
    campaign_idle_seconds: int = 30
    campaign_recipe: str = ""
    fixture_notes: str = ""
    recovery_attempts: int = 3
    restart_before_run: bool = False

    masters: list[Device] | None = None

    def master_devices(self):
        if self.masters is None:
            self.masters = [self.master] if self.master.identity or self.master.uart else []
        return self.masters

    def select_master(self):
        selected = [device for device in self.master_devices() if device.enabled]
        self.master = selected[0] if len(selected) == 1 else Device("", "Master", enabled=False)
        return selected

    def workload(self):
        rates = self.rates or [0]
        selected = [rates[0]]
        selection = "first configured rate"
        if self.preset == "soak":
            selected, selection = [rates[len(rates) // 2]], "single rate: middle entry of the configured list; no sweep"
        elif self.preset == "mixed-soak":
            selected, selection = [min(rates), max(rates)], "G1 typing bursts and G2 continuous pointer at independent rates"
        elif self.preset in ("baseline", "limit", "usb-stall"):
            selected, selection = sorted(set(rates)), "sweep configured rates"
        elif self.preset == "overload":
            selected, selection = [max(rates), min(rates)], "maximum rate followed by minimum-rate recovery"
        elif self.preset == "overlap": selected, selection = [2], "fixed overlap rate"
        elif self.preset == "resilience": selected = [min(rates[0], 25)]
        elif self.preset == "clipboard": selection = "dongle-only synthetic file, stalled receive, cancellation and input recovery"
        elif self.preset == "full":
            selected, selection = sorted(set(rates + [2, min(rates[0], 25)])), "everyday, overlap, resilience and configured-rate sweep"
        elif self.preset == "all-tests":
            selected = sorted(set(rates + [2, min(rates), min(min(rates), 25)]))
            selection = "timed loop: all compatible tests, rotating receivers, sources and rates; baseline requires different wiring"
        count = sum(d.enabled for d in self.generators)
        combined = [sum(selected)] if self.preset == "mixed-soak" else [rate * count for rate in selected]
        campaign = None
        if self.preset == "usb-stall":
            from .campaign import cases
            plan = cases(self)
            selected = sorted({case.rate for case in plan})
            combined = sorted({case.rate * case.sources for case in plan})
            campaign = {"cases": len(plan), "repeat_limit": self.campaign_repeats,
                        "planned_active_seconds": sum(c.seconds for c in plan),
                        "planned_idle_seconds": sum(c.idle_seconds for c in plan), "replay": bool(self.campaign_recipe)}
            selection = f"{len(plan)} USB cases; up to {self.campaign_repeats} attempts for failing recipes"
        segment_seconds = self.seconds
        if self.preset == "overlap":
            segment_seconds = 2
        elif self.preset == "clipboard":
            segment_seconds = max(self.seconds, 15)
        elif self.preset == "resilience":
            segment_seconds = resilience_seconds(self.seconds, selected[0])
        elif self.preset == "background":
            segment_seconds = max(self.seconds, 3)
        elif self.preset == "mixed-soak":
            segment_seconds = self.soak_seconds
        return {"preset": self.preset, "generator_count": count, "rates_per_generator": selected,
                "combined_rates": combined, "rate_selection": selection,
                "receiver_slots": [] if self.preset == "baseline" else [d.slot for d in self.receivers if d.enabled],
                "segment_seconds": segment_seconds,
                "soak_seconds": self.soak_seconds if self.preset in ("soak", "all-tests") else None,
                "recovery_attempts": self.recovery_attempts if self.preset == "all-tests" else None,
                "stop_on_failure": self.stop_on_failure, "campaign": campaign}

    def workload_text(self):
        w = self.workload()
        rates = ", ".join(map(str, w["rates_per_generator"]))
        combined = ", ".join(map(str, w["combined_rates"]))
        targets = ", ".join(map(str, w["receiver_slots"])) or "direct USB"
        if self.preset == "mixed-soak":
            typing, pointer = w["rates_per_generator"]
            return (f"mixed-soak: G1 typing bursts at {typing}/s; G2 pointer at {pointer}/s; "
                    f"targets {targets}; {self.campaign_idle_seconds}s idle then "
                    f"{w['segment_seconds']}s continuous input per receiver")
        duration_label = "total duration" if self.preset == "all-tests" else "soak"
        soak = f"; {duration_label} {w['soak_seconds']}s" if w["soak_seconds"] is not None else ""
        return (f"{w['preset']}: {w['generator_count']} generators; {rates} reports/s each ({combined} combined); "
                f"{w['rate_selection']}; targets {targets}; segments {w['segment_seconds']}s{soak}; "
                f"stop on failure {'on' if w['stop_on_failure'] else 'off'}")

    def validate(self):
        baseline = self.preset == "baseline"
        if not baseline and len(self.select_master()) != 1:
            raise ValueError("Select exactly one master in the Use column before running")
        if not baseline and (not self.master.uart or not re.fullmatch(r"[0-9A-Fa-f]{12}", self.master.identity)):
            raise ValueError("Discover the master and save its firmware identity before running")
        generators = [d for d in self.generators if d.enabled]
        receivers = [] if baseline else [d for d in self.receivers if d.enabled]
        if not 1 <= len(generators) <= 2 or (not baseline and not 1 <= len(receivers) <= 7):
            raise ValueError("Enable 1–2 generators and 1–7 receivers")
        if self.preset == "clipboard" and len(receivers) < 2:
            raise ValueError("Clipboard stress requires at least two enabled receivers")
        if self.preset == "mixed-soak" and len(generators) != 2:
            raise ValueError("Mixed soak requires two enabled generators")
        for d in generators + receivers:
            if type(d.enabled) is not bool: raise ValueError("Device enabled flags must be boolean")
            if not re.fullmatch(r"[0-9A-Fa-f]{12}", d.identity):
                raise ValueError(f"{d.name}: identity must be the device's 12-digit USB serial")
        devices = generators + receivers + ([] if baseline else [self.master])
        if len({d.identity.upper() for d in devices}) != len(devices):
            raise ValueError("Every device needs a distinct chip identity")
        ports = ([] if baseline else [self.master.uart]) + [d.uart for d in generators]
        if not all(ports) or len(set(ports)) != len(ports):
            raise ValueError("Master and generators require separate persistent UART paths")
        slots = [d.slot for d in receivers]
        if any(type(s) is not int or not 1 <= s <= 7 for s in slots) or len(set(slots)) != len(slots):
            raise ValueError("Enabled receiver slots must be unique numbers from 1 to 7")
        if self.preset not in PRESETS or not self.rates or any(type(r) is not int or not 1 <= r <= 1000 for r in self.rates):
            raise ValueError("Select a known preset and report rates between 1 and 1000 per generator")
        if type(self.seconds) is not int or type(self.soak_seconds) is not int or not 1 <= self.seconds <= 3600 or not 1 <= self.soak_seconds <= 604800:
            raise ValueError("Segment duration must be 1–3600 seconds; soak duration 1–604800")
        if type(self.seed) is not int or not 0 <= self.seed <= 0xffffffff or not 16 <= self.max_log_mb <= 65536:
            raise ValueError("Seed must be uint32; log budget must be 16–65536 MB")
        if type(self.stop_on_failure) is not bool: raise ValueError("Stop-on-failure must be boolean")
        if type(self.restart_before_run) is not bool: raise ValueError("Restart-before-run must be boolean")
        self.validate_campaign()

    def validate_campaign(self):
        if self.preset == "mixed-soak" and self.soak_seconds > MAX_MIXED_SOAK_SECONDS:
            raise ValueError(f"Mixed soak duration per receiver must be at most {MAX_MIXED_SOAK_SECONDS} seconds")
        for key, low, high in (("campaign_repeats", 1, 10), ("campaign_idle_seconds", 0, 3600), ("recovery_attempts", 1, 10)):
            value = getattr(self, key)
            if type(value) is not int or not low <= value <= high: raise ValueError(f"Invalid {key}")
        if not isinstance(self.fixture_notes, str) or len(self.fixture_notes) > 1024:
            raise ValueError("Fixture notes must be at most 1024 characters")
        if not isinstance(self.campaign_recipe, str) or len(self.campaign_recipe) > 16384:
            raise ValueError("Campaign recipe must be at most 16384 characters")
        if self.preset == "usb-stall":
            from .campaign import cases
            cases(self)
        if self.preset == "all-tests":
            if self.campaign_recipe:
                raise ValueError("Clear the USB replay recipe before selecting All tests")
            from .suite import plan
            plan(self)

    @classmethod
    def load(cls, path):
        data = json.loads(Path(path).read_text())
        if data.pop("version", None) != 1:
            raise ValueError("Unsupported profile version")
        for key in ("master", "generators", "receivers"):
            if key not in data:
                raise ValueError(f"Profile missing {key}")
        data["master"] = Device(**data["master"])
        for key in ("generators", "receivers", "masters"):
            if data.get(key) is None:
                continue
            data[key] = [Device(**item) for item in data[key]]
        return cls(**data)

    def save(self, path):
        destination = Path(path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(json.dumps({"version": 1, **asdict(self)}, indent=2) + "\n")
        temporary.replace(destination)


def pattern(source, run, sequence, seed, profile):
    """Independent implementation of the published 54-byte generator format."""
    if source not in (1, 2) or profile not in PROFILES.values():
        raise ValueError("Unsupported generator source or profile")
    keys = modifiers = buttons = x = y = wheel = pan = 0
    if sequence:
        first = 4 + (source - 1) * 16
        if profile == 3:
            keys = (1 << 4) if sequence % 2 else 0
        elif profile == 1:
            keys = (sequence & 0xffff) << first
        elif sequence % 2 and (profile in (0, 4) or
                               (profile == MIXED_PROFILE and source == 1 and sequence % 64 < 16)):
            keys = 1 << (first + sequence // 2 % 16)
            if sequence // 2 % 7 == 0:
                modifiers = 2 if source == 1 else 32
        if profile != MIXED_PROFILE or source == 2:
            buttons = (1 if profile == 3 else (1 << (source - 1))) if sequence // 4 % 2 else 0
            value = (sequence * 1664525 + seed * 1013904223) & 0xffffffff
            distance = (value >> 16) % 31 + 1
            distance *= -1 if sequence // 32 % 2 else 1
            if source == 1:
                x, wheel = distance, 1 if sequence % 2 else -1
            else:
                y, pan = distance, 1 if sequence % 2 else -1
    consumer = (0xe9 if source == 1 else 0xea) if sequence % 2 and profile == 4 else 0
    return (bytes([modifiers]) + keys.to_bytes(32, "little") +
            struct.pack("<HhhbbH2sBII", buttons, x, y, wheel, pan, consumer, b"KM", source, run, sequence))


def projection(data, source):
    keys = int.from_bytes(data[1:33], "little")
    return struct.pack("<HB", (keys >> (4 + (source - 1) * 16)) & 0xffff,
                       data[0] & (2 if source == 1 else 32))


@dataclass
class Stream:
    crc: int = 0
    count: int = 0
    previous: bytes = b""

    def add(self, value, changes=False):
        if changes and value == self.previous:
            return
        self.previous = value
        self.count += 1
        self.crc = zlib.crc32(value, self.crc)

    def summary(self):
        return {"count": self.count, "crc32": f"{self.crc:08x}"}


class Verifier:
    """Bounded-memory stream comparisons, independently projected by source.

    Mixed profiles change keyboard state on every sample, so firmware cannot
    legitimately coalesce their mouse reports. Source 1 owns X/wheel; source 2
    owns Y/pan. Pointer-only mode instead checks signed movement aggregates.
    """
    def __init__(self, sources, profile, seed=1):
        self.sources = tuple(sources)
        self.profile = profile
        self.coalescing = profile in (2, MIXED_PROFILE)
        self.seed = seed
        self.next_keys = {s: 1 for s in self.sources}
        self.keys = {s: Stream(previous=b"\0\0\0") for s in self.sources}
        self.buttons = {s: Stream(previous=b"\0") for s in self.sources}
        self.moves = {s: Stream() for s in self.sources}
        self.consumer = {s: Stream(previous=b"\0") for s in self.sources}
        self.totals = {s: [0, 0, 0] for s in self.sources}  # positive, negative, scroll
        self.errors = []
        self.state = {0: bytes(34), 1: bytes(8), 2: b""}
        self.reports = 0
        self.keyboard_full = Stream(previous=bytes(34))

    def error(self, message):
        if len(self.errors) < 20:
            self.errors.append(message)

    def feed(self, interface, raw):
        self.reports += 1
        if interface == 0:
            if len(raw) != 34:
                self.error(f"Keyboard report length {len(raw)}; expected 34 (report protocol)"); return
            if raw == self.state[0]:
                if any(raw): self.error("Duplicate non-neutral keyboard report")
                return
            self.state[0] = raw
            self.keyboard_full.add(raw)
            keybits = int.from_bytes(raw[2:], "little")
            allowed = sum(0xffff << (4 + (s - 1) * 16) for s in self.sources)
            mods = sum(2 if s == 1 else 32 for s in self.sources)
            if keybits & ~allowed or raw[0] & ~mods:
                self.error("Unexpected key/modifier outside configured source assignments")
            data = raw[:1] + raw[2:]
            for source in self.sources:
                value = projection(data, source)
                if value != self.keys[source].previous and self.profile not in (2, MIXED_PROFILE):
                    wanted = projection(pattern(source, 1, self.next_keys[source], self.seed, self.profile), source)
                    # The final all-up report has no workload sequence number.
                    # Its exact position is checked against completion counts.
                    if value != wanted and value != bytes(3): self.error(f"Source {source}: first keyboard divergence near sample {self.next_keys[source]}")
                    self.next_keys[source] += 1
                self.keys[source].add(value, changes=True)
        elif interface == 1:
            if len(raw) != 8:
                self.error(f"Mouse report length {len(raw)}; expected 8"); return
            self.state[1] = raw
            buttons, x, y, wheel, pan = struct.unpack("<Hhhbb", raw)
            if buttons & ~sum(1 << (s - 1) for s in self.sources):
                self.error("Unexpected mouse button")
            if (1 not in self.sources and (x or wheel)) or (2 not in self.sources and (y or pan)):
                self.error("Unexpected movement or scroll on an inactive source's axis")
            if self.profile == MIXED_PROFILE and (x or wheel):
                self.error("Mixed soak typing source produced pointer motion or scroll")
            for source in self.sources:
                button = (buttons >> (source - 1)) & 1
                self.buttons[source].add(bytes([button]), changes=True)
                distance, scroll = (x, wheel) if source == 1 else (y, pan)
                if distance or scroll:
                    value = struct.pack("<hbB", distance, scroll, button)
                    if not self.coalescing:
                        expected = pattern(source, 1, self.moves[source].count + 1, self.seed, self.profile)
                        desired = struct.pack("<hbB", struct.unpack_from("<h", expected, 35 if source == 1 else 37)[0],
                                              struct.unpack_from("<b", expected, 39 if source == 1 else 40)[0],
                                              (int.from_bytes(expected[33:35], "little") >> (source - 1)) & 1)
                        if value != desired: self.error(f"Source {source}: first mouse divergence near sample {self.moves[source].count + 1}")
                    self.moves[source].add(value)
                    self.totals[source][0] += max(distance, 0)
                    self.totals[source][1] += min(distance, 0)
                    self.totals[source][2] += scroll
        elif interface == 2:
            if not raw or raw[0] not in (1, 2) or len(raw) != (9 if raw[0] == 1 else 2):
                self.error("Malformed control report"); return
            self.state[2] = raw
            if raw[0] == 1:
                usages = set(struct.unpack("<4H", raw[1:])) - {0}
                allowed = {0xe9 if s == 1 else 0xea for s in self.sources} if self.profile == 4 else set()
                if usages - allowed: self.error("Unexpected media control")
                for source in self.sources:
                    self.consumer[source].add(bytes([int((0xe9 if source == 1 else 0xea) in usages)]), changes=True)
            elif any(raw[1:]): self.error("Unexpected system control")
        else:
            self.error(f"Unexpected interface {interface}")

    def finish(self, run, seed, counts, check=None):
        comparisons = {}
        for source in self.sources:
            expected = Verifier([source], self.profile, seed)
            payload_crc = 0
            for sequence in range(1, counts[source] + 1):
                if check and sequence % 4096 == 0:
                    check()
                data = pattern(source, run, sequence, seed, self.profile)
                payload_crc = zlib.crc32(data, payload_crc)
                expected.feed(0, data[:1] + b"\0" + data[1:33])
                expected.feed(1, data[33:41])
                expected.feed(2, b"\x01" + data[41:43] + bytes(6))
            expected.feed(0, bytes(34)); expected.feed(1, bytes(8)); expected.feed(2, b"\x01" + bytes(8))
            for name in ("keys", "buttons", "moves", "consumer"):
                actual = getattr(self, name)[source].summary()
                wanted = getattr(expected, name)[source].summary()
                if name == "moves" and self.coalescing:
                    actual, wanted = self.totals[source], expected.totals[source]
                comparisons[f"source_{source}_{name}"] = {"actual": actual, "expected": wanted}
                if actual != wanted: self.error(f"Source {source}: {name} stream mismatch")
            comparisons[f"source_{source}_payload_crc"] = f"{payload_crc:08x}"
        if any(self.state[0]) or any(self.state[1][:2]) or any(s.previous != b"\0" for s in self.consumer.values()):
            self.error("Input still held at segment end")
        return {"errors": list(self.errors), "reports": self.reports, "comparisons": comparisons,
                "actual_keyboard": self.keyboard_full.summary(),
                "mouse_check": "signed aggregates" if self.coalescing else "ordered per-source reports"}


def trace_page(data):
    if len(data) != 128 or data[:4] != b"KMT1" or zlib.crc32(data[:124]) != int.from_bytes(data[124:], "little"):
        raise ValueError("Corrupt or unsupported trace page")
    boot, run, count, overwritten, flags, index = struct.unpack_from("<6I", data, 4)
    if flags & 4: raise ValueError("Device could not allocate its diagnostic trace buffer")
    if not flags & 2:
        raise ValueError("Trace must be frozen before download")
    record = None
    if index < count:
        sequence, micros, stage, a, b, c, length = struct.unpack_from("<7I", data, 28)
        if sequence != index or length > 64: raise ValueError("Invalid trace record identity/length")
        record = {"sequence": sequence, "micros": micros, "stage": stage, "a": a, "b": b, "c": c,
                  "data": data[56:56 + length].hex()}
    result = {"boot": f"{boot:08x}", "run": run, "records_total": count,
              "overwritten": overwritten, "index": index, "record": record}
    if index == 0xffffffff:
        if data[28:32] != b"SUM1": raise ValueError("Missing trace summary")
        names = ("routed_keys_count", "routed_keys_crc", "reference_keys_count", "reference_keys_crc",
                 "completed_keys_count", "completed_keys_crc", "source1_count", "source1_crc", "source2_count", "source2_crc",
                 "faults", "injections", "raw", "decoded", "routed", "tx", "rx", "accepted", "rejected", "queued", "submitted", "completed")
        result["summary"] = dict(zip(names, struct.unpack_from("<22I", data, 32)))
    return result


def handovers(slots):
    return [(a, b) for a in slots for b in slots if a != b]


class OverlapVerifier:
    """Shared-key behavior is checked against the master's actual ingress order."""
    def __init__(self, sources):
        self.sources = tuple(sources)
        self.keyboard = Stream(previous=bytes(34))
        self.mouse_buttons = Stream(previous=bytes(2))
        self.errors, self.reports = [], 0
        self.held = {0: False, 1: False}

    def feed(self, interface, raw):
        self.reports += 1
        if interface == 0:
            if len(raw) != 34 or raw[0] or int.from_bytes(raw[2:], "little") & ~(1 << 4):
                self.errors.append("Unexpected shared-key report"); return
            self.held[0] = any(raw)
            if raw != self.keyboard.previous or any(raw): self.keyboard.add(raw)
        elif interface == 1:
            if len(raw) != 8: self.errors.append("Malformed overlap mouse report"); return
            self.held[1] = any(raw[:2])
            if int.from_bytes(raw[:2], "little") & ~1: self.errors.append("Unexpected shared mouse button")
            self.mouse_buttons.add(raw[:2], changes=True)
        elif interface == 2 and any(raw[1:]): self.errors.append("Unexpected overlap control report")

    def finish(self, run, seed, counts):
        comparisons = {}
        for source in self.sources:
            crc = 0
            for sequence in range(1, counts[source] + 1): crc = zlib.crc32(pattern(source, run, sequence, seed, 3), crc)
            comparisons[f"source_{source}_payload_crc"] = f"{crc:08x}"
        if any(self.held.values()): self.errors.append("Shared controls still held at end")
        return {"errors": self.errors[:20], "reports": self.reports, "comparisons": comparisons,
                "actual_keyboard": self.keyboard.summary(), "actual_buttons": self.mouse_buttons.summary(),
                "mouse_check": "shared-button edges; mixed profiles verify motion"}


def verify_overlap(result, master, counts):
    retained_raw = sum(record["stage"] == 1 for record in master.get("records", []))
    if ("error" in master or master["overwritten"] or
            master.get("summary", {}).get("raw", retained_raw) > retained_raw):
        # Fault latching can stop the detailed window while summary counters
        # continue. A partial arrival order is not a valid overlap oracle.
        result["overlap_reference_incomplete"] = True
        result["errors"].append("Shared-key reference needs a complete master ingress trace"); return
    states, seen = {}, {source: [] for source in counts}
    expected = Stream(previous=bytes(34))
    expected_buttons = Stream(previous=bytes(2))
    simultaneous_key = simultaneous_button = False
    for record in master["records"]:
        raw = bytes.fromhex(record["data"])
        if record["stage"] != 1 or len(raw) != 55 or raw[0] != 1: continue
        data = raw[1:]
        if data[43:45] != b"KM": continue
        source = data[45]; run, sequence = struct.unpack_from("<II", data, 46)
        if run != result["run"] or source not in seen: continue
        states[source] = data
        if len(states) == 2:
            simultaneous_key |= all(state[1] & (1 << 4) for state in states.values())
            simultaneous_button |= all(state[33] & 1 for state in states.values())
        if sequence: seen[source].append(sequence)
        keys = 0; modifiers = 0; buttons = 0
        for state in states.values():
            keys |= int.from_bytes(state[1:33], "little"); modifiers |= state[0]
            buttons |= int.from_bytes(state[33:35], "little")
        expected.add(bytes([modifiers, 0]) + keys.to_bytes(32, "little"), changes=True)
        expected_buttons.add(struct.pack("<H", buttons), changes=True)
    for source, count in counts.items():
        if seen[source] != list(range(1, count + 1)):
            result["errors"].append(f"Shared-key source {source}: missing/reordered master ingress")
    result["overlap_evidence"] = {"key": simultaneous_key, "button": simultaneous_button}
    result["expected_keyboard"] = expected.summary()
    if result["actual_keyboard"] != result["expected_keyboard"]:
        result["errors"].append("Shared-key merge/release differed from independent ingress reference")
    result["expected_buttons"] = expected_buttons.summary()
    if result["actual_buttons"] != result["expected_buttons"]:
        result["errors"].append("Shared-button merge/release differed from independent ingress reference")
