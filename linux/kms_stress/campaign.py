"""Finite USB-stall experiments, saved recipes and bounded repeat attempts."""
from dataclasses import asdict, dataclass
import hashlib
import json
import secrets
import time

from . import usb_diagnostics as usb
from .diagnostics import GeneratorStopError, WorkloadStalled


MAX_CASES = 1200
ACTIVITIES = (
    ("stress", "input"), ("stress", "idle-wake"), ("stress", "led"),
    ("stress", "background"), ("stress", "console"), ("pointer", "background"),
    ("media", "input"), ("stress", "receiver-readiness"),
)


@dataclass(frozen=True)
class Case:
    slot: int
    rate: int
    seconds: int
    seed: int
    sources: int
    profile: str
    activity: str
    idle_seconds: int = 0
    first_source: int = 1

    def validate(self):
        for key, low, high in (("slot", 1, 7), ("rate", 1, 1000), ("seconds", 1, 3600),
                               ("seed", 0, 0xffffffff), ("sources", 1, 2), ("idle_seconds", 0, 3600), ("first_source", 1, 2)):
            value = getattr(self, key)
            if type(value) is not int or not low <= value <= high: raise ValueError(f"Invalid campaign {key}")
        if self.profile not in ("stress", "pointer", "media") or self.activity not in ("input", "idle-wake", "led", "background", "console", "receiver-readiness"):
            raise ValueError("Unsupported USB campaign activity/profile")
        if self.first_source + self.sources > 3: raise ValueError("Campaign source range exceeds two generators")
        return self

    @property
    def source_ids(self):
        return range(self.first_source, self.first_source + self.sources)

    @property
    def identity(self):
        return hashlib.sha256(json.dumps(asdict(self), sort_keys=True).encode()).hexdigest()[:16]


def fixture(config):
    return {"master": config.master.identity.upper(),
            "generators": [d.identity.upper() for d in config.generators if d.enabled],
            "receivers": sorted([d.slot, d.identity.upper()] for d in config.receivers if d.enabled)}


def cases(config):
    count = sum(d.enabled for d in config.generators)
    if config.campaign_recipe:
        try:
            recipe = json.loads(config.campaign_recipe)
            case = Case(**recipe["case"]).validate()
        except (TypeError, KeyError) as error: raise ValueError("Malformed campaign recipe") from error
        if recipe.get("version") != 1 or recipe.get("fixture") != fixture(config):
            raise ValueError("Replay recipe fixture does not match the configured chip identities/slots")
        if (recipe.get("case_id") != case.identity or max(case.source_ids) > count or
                case.slot not in [d.slot for d in config.receivers if d.enabled]):
            raise ValueError("Replay recipe identity/source count mismatch")
        return [case]
    slots = sorted(d.slot for d in config.receivers if d.enabled)
    rates = sorted(set(config.rates))
    source_combinations = count * (count + 1) // 2
    if len(slots) * len(rates) * source_combinations * len(ACTIVITIES) > MAX_CASES:
        raise ValueError(f"USB campaign exceeds {MAX_CASES} cases; reduce rates or enabled receivers")
    return [
        Case(slot, rate, config.seconds, config.seed, sources, profile, activity,
             config.campaign_idle_seconds if activity == "idle-wake" else 0, first_source)
        for slot in slots for rate in rates for sources in range(1, count + 1)
        for profile, activity in ACTIVITIES for first_source in range(1, count - sources + 2)
    ]


class USBCollector:
    def __init__(self, runner):
        self.runner = runner
        self.token = secrets.randbelow(0xfffffffe) + 1
        self.before = None
        self.samples = []
        self.next_poll = 0

    def snapshot(self):
        self.runner.check()
        self.token = self.token % 0xffffffff + 1
        token = self.token
        # A fault window shares this UART with the status reply. Keep a bounded
        # wait, servicing generator leases while the retained dump drains.
        lines = self.runner.master.request(f"usb-status {token}", f"KMSUSBSTATUS END id={token}",
                                           timeout=8, service=self.runner.service_workload)
        sample = usb.snapshot(lines, token)
        self.runner.check()
        # Test-trace and diagnostics initialize independent boot IDs. Compare
        # each namespace with itself; retain the USB baseline across cases.
        expected = self.runner.expected_usb_boot
        if expected is not None and sample["boot"] != expected:
            raise RuntimeError("Master boot changed during USB campaign")
        if expected is None: self.runner.expected_usb_boot = sample["boot"]
        if self.samples and sample["boot"] == self.samples[-1]["boot"]:
            previous = self.samples[-1]["at"]
            if sample["at"] != previous and not usb.newer(sample["at"], previous): raise ValueError("Stale USB snapshot")
        self.samples.append(sample)
        if len(self.samples) > 256: self.samples.pop(0)
        self.runner.evidence.log("usb-snapshot", {"case_id": self.runner.campaign_case.identity, "snapshot": sample})
        return sample

    def poll(self):
        if time.monotonic() >= self.next_poll:
            sample = self.snapshot()
            self.next_poll = time.monotonic() + 5
            if self.before and usb.newer(sample["fault"], self.before["fault"]):
                raise WorkloadStalled("New S3 USB fault during campaign workload")
            return True
        return False

    def frozen(self):
        errors = []
        try:
            lines = self.runner.master.request("usb-fault", ("KMSUSBFAULT END", "KMSUSBFAULT NONE", "KMSUSBFAULT BUSY"),
                                               timeout=8, service=self.runner.service_workload)
            if lines[-1] == "KMSUSBFAULT BUSY": raise RuntimeError("USB fault snapshot busy")
        except Exception as error:
            lines = []; errors.append(str(error))
        with self.runner.evidence.lock:
            retained = list(self.runner.evidence.usb_lines)
        frames = usb.fault_frames(retained + lines)
        unique = {(f.get("boot"), f["sequence"]): f for f in frames}
        return list(unique.values()), errors


def capture_failure(runner):
    # Freeze surviving trace windows before stopping traffic or reconnecting USB.
    errors = []
    try: runner.master.request("bench freeze", "KMSBENCH OK", timeout=1)
    except Exception as error: errors.append(str(error))
    for device in runner.config.receivers:
        if device.enabled:
            try: runner.capture.feature(device.slot, 2)
            except Exception as error: errors.append(str(error))
    for source in runner.generators:
        try: source.request("bench stop", "KMSGEN STOPPING", timeout=1)
        except Exception as error: errors.append(str(error))
    return errors


def healthy(runner, collector):
    runner.check()
    sample = collector.snapshot()
    if not collector.before or sample["boot"] != collector.before["boot"]: return False
    state = sample["state"]
    return (state["state"] == 0 and state["inputs"] == collector.before["state"]["inputs"] > 0 and
            not state["suspended"])


def prepare_repeat(runner, collector):
    """Only reconnect the isolated generator input bus, never reset a board/PC."""
    runner.check()
    runner.master.request("pause")
    runner.master.request("bench drop 0", "KMSBENCH DROP 0")
    for device in runner.config.receivers:
        if device.enabled:
            runner.capture.feature(device.slot, 5, 0)
            runner.capture.feature(device.slot, 6, 0)
            runner.capture.leds(device.slot, 0)
    reconnected = False

    def reconnect(reason):
        nonlocal reconnected
        reconnected = True
        runner.evidence.log("campaign-recovery", {"case_id": runner.campaign_case.identity, "reason": reason})
        runner.master.request("usb-recover", "KMSUSB RESULT", timeout=3)
        deadline = time.monotonic() + 15
        while not healthy(runner, collector):
            if time.monotonic() >= deadline: raise RuntimeError("USB did not recover; remaining repeats were not run")
            runner.wait(0.5)

    # Matching input counts alone do not prove that a generator's cancelled
    # transfer was retired after the host's automatic recovery.
    if not healthy(runner, collector): reconnect("unhealthy USB inputs")
    try: runner.stop_generators()
    except GeneratorStopError as error:
        runner.evidence.log("campaign-generator-stop", {"statistics": error.stats, "errors": error.errors})
        if not error.incomplete or reconnected: raise
        reconnect("generator neutral release incomplete after host recovery")
        runner.stop_generators()
    from .runner import verify_inventory
    verify_inventory(runner.master.request("usb-list", "KMSUSB END", timeout=3),
                     [d for d in runner.config.generators if d.enabled])


def run(runner, plan=None):
    from .runner import DurationReached
    plan = cases(runner.config) if plan is None else plan
    summaries = []
    runner.campaign_summary = summaries
    runner.evidence.save("campaign-plan.json", {"version": 1, "fixture": fixture(runner.config),
        "fixture_notes": runner.config.fixture_notes, "cases": [{"case_id": c.identity, **asdict(c)} for c in plan],
        "repeat_limit": runner.config.campaign_repeats,
        "limits": ["Synthetic HID; fixed descriptors/poll intervals", "Receiver readiness override is not a Windows reboot or electrical disconnect",
                   "Workload seed does not reproduce USB/ISR/radio scheduling", "No automatic board reset or capture re-arm"]})
    for case in plan:
        runner.campaign_case = case
        recipe = {"version": 1, "fixture": fixture(runner.config), "case_id": case.identity, "case": asdict(case)}
        runner.evidence.save(f"recipe-{case.identity}.json", recipe)
        summary = {"case_id": case.identity, "maximum_attempts": runner.config.campaign_repeats,
                   "attempts": [], "not_run": 0}
        summaries.append(summary)
        for attempt in range(1, runner.config.campaign_repeats + 1):
            runner.begin_progress_step(case.seconds + case.idle_seconds,
                                       f"USB {case.activity}, attempt {attempt}", case.slot, extra=attempt > 1)
            if runner.config.preset == "all-tests" and runner.duration_reached():
                summary["not_run"] = runner.config.campaign_repeats - attempt + 1
                summary["duration_reached"] = True
                break
            collector = USBCollector(runner)
            runner.campaign_collector = collector
            index = len(runner.completed_segments) + 1
            try:
                collector.before = collector.snapshot()
                state = collector.before["state"]
                if state["state"] != 0 or not state["inputs"] or state["suspended"]:
                    raise RuntimeError("USB campaign requires healthy inputs before each attempt")
            except Exception as error:
                summary.update(not_run=runner.config.campaign_repeats - attempt + 1, repeat_blocked=str(error))
                runner.evidence.save("campaign-summary.json", {"cases": summaries})
                raise
            item, capture_errors, availability = None, [], None
            try:
                runner.notify(f"USB case {case.identity}, attempt {attempt}: {case.activity}, {case.sources} sources at {case.rate}/s")
                if case.activity == "receiver-readiness":
                    # USB-not-ready peers remain selectable; unpaired slots do not.
                    runner.master.request(f"select {case.slot}", f"KMSROUTE SELECTED {case.slot}")
                    availability = runner.availability_check(case.slot)
                    runner.evidence.log("campaign-readiness", {"case_id": case.identity, **availability})
                    if not availability["passed"]: raise WorkloadStalled("Receiver readiness/selection check failed")
                runner.wait(min(case.idle_seconds, runner.remaining_seconds()) if runner.config.preset == "all-tests" else case.idle_seconds)
                if runner.config.preset == "all-tests" and runner.duration_reached():
                    summary["duration_reached"] = True
                    break
                item = runner.segment(index, case.slot, case.rate, case.seconds, case.profile, campaign_case=case)
            except (WorkloadStalled, GeneratorStopError) as error:
                capture_errors.extend(capture_failure(runner))
                traces = runner.collect_traces()
                runner.evidence.save(f"trace-{index:05d}.json", traces)
                item = runner.interrupted_segment(error, traces)
                if item is None:
                    item = dict(slot=case.slot, rate=case.rate, seconds=case.seconds, profile=case.profile,
                                status="FAIL", partial=True, errors=[str(error)])
            except DurationReached:
                summary["duration_reached"] = True
                break
            except Exception as error:
                capture_errors.extend(capture_failure(runner))
                from .runner import Cancelled
                if isinstance(error, Cancelled): raise
                traces = runner.collect_traces()
                runner.evidence.save(f"trace-{index:05d}.json", traces)
                item = runner.interrupted_segment(error, traces)
                if item is None:
                    item = dict(slot=case.slot, rate=case.rate, seconds=case.seconds, profile=case.profile,
                                status="INCONCLUSIVE", partial=True, errors=[str(error)])
            finally:
                with runner.lock: runner.segment_open = False
            frozen, fault_errors = collector.frozen()
            capture_errors.extend(fault_errors)
            try: after = collector.snapshot()
            except Exception as error: after = None; capture_errors.append(str(error))
            classification = usb.classify(collector.before, after, frozen, item)
            if classification["signature"] not in ("no-failure",) and item["status"] in ("PASS", "LIMITED"):
                item["status"] = "FAIL"; item["errors"].append("USB campaign observed " + classification["signature"])
            if after is None or capture_errors or classification["confidence"] == "incomplete":
                item["status"] = "INCONCLUSIVE"
            item.update(case_id=case.identity, attempt=attempt, recipe=recipe, usb_classification=classification,
                        usb_evidence_errors=capture_errors)
            if availability is not None: item["receiver_readiness"] = availability
            runner.completed_segments.append(item)
            runner.evidence.save(f"usb-{index:05d}.json", {"before": collector.before, "after": after,
                "samples": collector.samples, "frozen_faults": frozen, "errors": capture_errors})
            runner.evidence.log("campaign-attempt", item)
            runner.end_progress_step()
            summary["attempts"].append({"attempt": attempt, "status": item["status"], "signature": classification["signature"]})
            # Healthy cases run once. A failing recipe gets bounded repeated
            # attempts, regardless of the normal preset's stop-on-failure flag.
            if attempt == 1 and item["status"] in ("PASS", "LIMITED"): break
            if item["status"] == "INCONCLUSIVE":
                summary["not_run"] = runner.config.campaign_repeats - attempt; break
            if attempt < runner.config.campaign_repeats:
                try: prepare_repeat(runner, collector)
                except Exception as error:
                    from .runner import Cancelled
                    if isinstance(error, Cancelled): raise
                    summary["not_run"] = runner.config.campaign_repeats - attempt
                    summary["repeat_blocked"] = str(error); break
        counts = {}
        for item in summary["attempts"]:
            if item["status"] == "FAIL": counts[item["signature"]] = counts.get(item["signature"], 0) + 1
        summary["failure_signatures"] = counts
        summary["same_signature_seen_again"] = any(n > 1 for signature, n in counts.items()
                                                     if signature != "unclassified-failure")
        # The last failed repeat needs the same recovery gate before a different
        # recipe. Otherwise pending transfers become misleading config errors.
        if (counts and not runner.config.stop_on_failure and case != plan[-1] and
                not summary.get("repeat_blocked") and
                not any(a["status"] == "INCONCLUSIVE" for a in summary["attempts"])):
            try: prepare_repeat(runner, collector)
            except Exception as error:
                from .runner import Cancelled
                if isinstance(error, Cancelled): raise
                summary["repeat_blocked"] = str(error)
                summary["remaining_cases_not_run"] = len(plan) - len(summaries)
        runner.evidence.save("campaign-summary.json", {"cases": summaries})
        if summary.get("repeat_blocked") or any(a["status"] == "INCONCLUSIVE" for a in summary["attempts"]): break
        if counts and runner.config.stop_on_failure: break
    runner.campaign_collector = None
    return runner.completed_segments
