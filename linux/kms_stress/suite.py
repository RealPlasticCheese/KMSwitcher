"""Timed, interleaved fixture coverage using the existing verified test engine."""
from collections import Counter, deque
from dataclasses import asdict, dataclass, replace
import hashlib
import json
import time

from .campaign import Case, cases
from .hardware import Capture, read_identity
from .model import handovers, resilience_seconds


@dataclass(frozen=True)
class Test:
    name: str
    slot: int
    rate: int
    seconds: int
    profile: str = "everyday"
    task: str = ""
    overload: bool = False
    start_slot: int = 0
    usb: Case | None = None

    @property
    def identity(self):
        return hashlib.sha256(json.dumps(asdict(self), sort_keys=True).encode()).hexdigest()[:16]


def plan(config):
    """Interleave families so a short run does not spend its whole budget on one."""
    slots = sorted(d.slot for d in config.receivers if d.enabled)
    if not slots: return []
    rates = sorted(set(config.rates))
    low, high, seconds = rates[0], rates[-1], config.seconds
    resilience_rate = min(low, 25)
    groups = []
    for name, profile, task, rate, duration in (
        ("everyday", "everyday", "", low, seconds),
        ("pointer", "pointer", "", low, seconds),
        ("media", "media", "", low, seconds),
        ("overlap", "overlap", "", 2, 2),
        ("resilience", "everyday", "resilience", resilience_rate, resilience_seconds(seconds, resilience_rate)),
        ("background", "everyday", "background", low, max(seconds, 3)),
    ):
        groups.append(deque((Test(name, slot, rate, duration, profile, task),) for slot in slots))
    groups.append(deque((Test("routing", target, low, seconds, start_slot=source),)
                        for source, target in handovers(slots)))
    if len(slots) > 1:
        groups.append(deque([(Test("live-handover", slots[0], min(low, 50), seconds),)]))
        groups.append(deque((Test("clipboard", slot, low, max(seconds, 15), task="clipboard"),
                             Test("clipboard-recovery", slot, low, max(seconds, 3))) for slot in slots))
    groups.append(deque((Test("limit", slot, rate, seconds, "stress"),)
                        for rate in rates for slot in slots))
    # Always follow overload with a low-rate delivery check before other traffic.
    groups.append(deque((Test("overload", slot, high, seconds, "stress", overload=True),
                         Test("overload-recovery", slot, low, max(seconds, 3))) for slot in slots))
    groups.append(deque((Test("usb-stall", c.slot, c.rate, c.seconds, c.profile, usb=c),)
                        for c in cases(config)))
    result = []
    while any(groups):
        for group in groups:
            if group: result.extend(group.popleft())
    return result


def recover(runner):
    """Re-establish neutral sources, unchanged identities and fresh input queues."""
    from .runner import verify_inventory
    config = runner.config
    if runner.clipboard_load:
        runner.clipboard_load.close(); runner.clipboard_load = None
    master = read_identity(runner.master)
    if master.role != "master" or master.identity != config.master.identity.upper():
        raise RuntimeError("Master identity changed during recovery")
    devices = [d for d in config.generators if d.enabled]
    for channel, device in zip(runner.generators, devices):
        identity = read_identity(channel)
        if identity.role != "generator" or identity.identity != device.identity.upper():
            raise RuntimeError("Generator identity changed during recovery")
    runner.master.request("pause")
    runner.master.request("bench off", "KMSBENCH OK")
    runner.master.request("test-mode on", "KMSTEST MODE test")
    runner.expected_master_boot = master.boot
    runner.master.restarted = False
    runner.expected_usb_boot = None
    runner.campaign_collector = runner.campaign_case = None
    try:
        runner.stop_generators()
        verify_inventory(runner.master.request("usb-list", "KMSUSB END", timeout=5), devices)
    except Exception:
        runner.master.request("usb-recover", "KMSUSB RESULT", timeout=3)
        until = time.monotonic() + 15
        while True:
            if runner.cancel.is_set():
                from .runner import Cancelled
                raise Cancelled("Stopped by operator")
            try:
                runner.stop_generators()
                verify_inventory(runner.master.request("usb-list", "KMSUSB END", timeout=3), devices)
                break
            except Exception:
                if time.monotonic() >= until: raise
                runner.cancel.wait(0.25)
    # Never discard evidence and resume an errored capture. Quiesce the sources,
    # then close the old queues and open newly verified receiver interfaces.
    runner.cancel.wait(1.2)
    if runner.capture:
        runner.capture.close()
        runner.capture = None
    runner.check()
    capture = Capture([d for d in config.receivers if d.enabled], runner.consume, runner.evidence.log)
    runner.capture = capture
    capture.open()
    for device in config.receivers:
        if device.enabled:
            capture.feature(device.slot, 3)
            capture.leds(device.slot, 0)
            capture.feature(device.slot, 1, 1)
            capture.feature(device.slot, 2)
            capture.page(device.slot, 0)
    with runner.lock:
        runner.segment_open = False
        runner.live_routes = None
        runner.verifier = None
        runner.leaks = []
    runner.active_segment = None
    runner.wait(0.25)


def save_failure(runner, test, error, index):
    from .campaign import capture_failure
    capture_errors = capture_failure(runner)
    traces = runner.collect_traces()
    runner.evidence.save(f"trace-{index:05d}.json", traces)
    item = runner.interrupted_segment(error, traces)
    if item is None:
        item = dict(slot=test.slot, rate=test.rate, seconds=test.seconds, profile=test.profile,
                    status="INCONCLUSIVE", partial=True, errors=[str(error)])
    item["capture_errors"] = capture_errors
    runner.completed_segments.append(item)


def execute(runner, test):
    index = len(runner.completed_segments) + 1
    if test.usb:
        from .campaign import run as campaign
        case = replace(test.usb, seed=runner.config.seed)
        campaign(runner, [case])
        return
    if test.name == "live-handover":
        item = runner.live_handover(index)
    else:
        if test.start_slot:
            runner.master.request(f"select {test.start_slot}", f"KMSROUTE SELECTED {test.start_slot}")
            runner.master.request("pause")
            runner.wait(0.15)
        item = runner.segment(index, test.slot, test.rate, test.seconds, test.profile,
                              test.overload, test.task)
    if item is not None: runner.completed_segments.append(item)


def run(runner):
    from .runner import Cancelled, DurationReached
    config = runner.config
    tests = plan(config)
    started = time.monotonic()
    runner.timed_deadline = started + config.soak_seconds
    if runner.run_progress: runner.run_progress.start_timed(runner.timed_deadline)
    base_seed = config.seed
    dual_source = sum(d.enabled for d in config.generators) >= 2
    summary = {"duration_seconds": config.soak_seconds, "completed_cycles": 0, "cycle": 0,
               "stop_reason": "duration", "counts": {}, "coverage": {}, "recoveries": [],
               "excluded": ["Baseline requires generator native USB connected directly to Linux",
                            "Physical power cycling, electrical hot-plug, Windows/BIOS and real keyboards require separate fixtures"],
               "additional_checks": ["Each generator alone and both together" if dual_source else "Single-source input", "Idle/wake and LED feedback",
                                     "Low-rate recovery after overload", "Every directed receiver pair"],
               "plan": [{"test_id": test.identity, **asdict(test)} for test in tests]}
    if not dual_source:
        summary["excluded"].append("Simultaneous two-source input and shared-key/button merging require two enabled generators")
    if len([d for d in config.receivers if d.enabled]) < 2:
        summary["excluded"].append("Dongle-only clipboard traffic requires two enabled receivers")
    runner.suite_summary = summary
    cycle = 0
    try:
        while not runner.duration_reached():
            cycle += 1
            summary["cycle"] = cycle
            config.seed = (base_seed + cycle - 1) & 0xffffffff
            completed_cycle = True
            for test in tests:
                if runner.cancel.is_set(): raise Cancelled("Stopped by operator")
                if runner.duration_reached():
                    completed_cycle = False
                    break
                before = len(runner.completed_segments)
                counts = dict(Counter(s["status"] for s in runner.completed_segments))
                runner.begin_progress_step(test.seconds, test.name, test.slot)
                runner.suite_progress = dict(running=True, cycle=cycle, test=test.name, slot=test.slot,
                                             rate=test.rate, counts=counts, duration_seconds=config.soak_seconds)
                runner.notify(f"Cycle {cycle}: {test.name}, receiver {test.slot}, {test.rate}/s; "
                              f"{runner.remaining_seconds():.0f}s remaining; " +
                              ", ".join(f"{k}={v}" for k, v in counts.items()))
                try:
                    runner.check()
                    execute(runner, test)
                except DurationReached:
                    runner.active_segment = None
                    completed_cycle = False
                    break
                except Cancelled:
                    raise
                except Exception as error:
                    save_failure(runner, test, error, before + 1)
                finally:
                    with runner.lock:
                        runner.segment_open = False
                        runner.live_routes = None
                    runner.campaign_collector = None
                items = runner.completed_segments[before:]
                complete_attempts = sum(not item.get("partial") for item in items)
                if not complete_attempts:
                    completed_cycle = False
                for item in items:
                    item.update(test=test.name, test_id=test.identity, cycle=cycle, seed=config.seed)
                coverage = summary["coverage"].setdefault(test.identity, dict(test=test.name, slot=test.slot,
                    rate=test.rate, profile=test.profile, activity=test.usb.activity if test.usb else test.task,
                    sources=list(test.usb.source_ids) if test.usb else list(range(1, len(runner.generators) + 1)),
                    attempts=0, completed_attempts=0, counts={}))
                coverage["attempts"] += len(items)
                coverage["completed_attempts"] += complete_attempts
                coverage["counts"] = dict(Counter(coverage["counts"]) + Counter(i["status"] for i in items))
                summary["counts"] = dict(Counter(s["status"] for s in runner.completed_segments))
                summary["remaining_seconds"] = runner.remaining_seconds()
                failed = any(i["status"] in ("FAIL", "INCONCLUSIVE", "OVERLOAD OBSERVED") for i in items)
                runner.evidence.save("suite-progress.json", summary)
                if failed:
                    if runner.duration_reached():
                        completed_cycle = False
                        break
                    if config.stop_on_failure and any(i["status"] in ("FAIL", "INCONCLUSIVE") for i in items):
                        summary["stop_reason"] = "stop on failure"
                        return
                    restored = False
                    for attempt in range(1, config.recovery_attempts + 1):
                        runner.notify(f"Recovery {attempt}/{config.recovery_attempts} after {test.name}")
                        record = dict(cycle=cycle, test_id=test.identity, attempt=attempt)
                        summary["recoveries"].append(record)
                        try:
                            recover(runner)
                            record["status"] = "recovered"
                            restored = True
                            break
                        except Cancelled:
                            raise
                        except Exception as error:
                            record.update(status="failed", error=str(error))
                            if runner.duration_reached(): break
                    if not restored:
                        summary["stop_reason"] = "recovery exhausted"
                        raise RuntimeError("Timed loop stopped: reliable capture and neutral input could not be restored")
            if completed_cycle: summary["completed_cycles"] += 1
    except Cancelled:
        summary["stop_reason"] = "cancelled"
        raise
    except Exception:
        if summary["stop_reason"] == "duration": summary["stop_reason"] = "unrecoverable error"
        raise
    finally:
        config.seed = base_seed
        for index, item in enumerate(runner.completed_segments):
            if item.get("requires_recovery_check"):
                followup = next((s for s in runner.completed_segments[index + 1:]
                                 if s.get("test") == "overload-recovery" and s.get("slot") == item.get("slot")
                                 and s.get("cycle") == item.get("cycle")), None)
                item["recovery_check_status"] = followup["status"] if followup else "not run"
                if followup is None:
                    item["status"] = "INCONCLUSIVE"
                    item["errors"].append("Overload recovery was not verified before this run ended")
        for test_id, coverage in summary["coverage"].items():
            coverage["counts"] = dict(Counter(s["status"] for s in runner.completed_segments if s.get("test_id") == test_id))
        summary["elapsed_seconds"] = time.monotonic() - started
        summary["counts"] = dict(Counter(s["status"] for s in runner.completed_segments))
        summary["not_run"], summary["incomplete"] = [], []
        for test in tests:
            coverage = summary["coverage"].get(test.identity, {})
            if coverage.get("completed_attempts"):
                continue
            group = "incomplete" if coverage.get("attempts") else "not_run"
            summary[group].append(dict(test_id=test.identity, test=test.name, slot=test.slot, rate=test.rate))
        runner.suite_progress = dict(running=False, cycle=cycle, test=summary["stop_reason"],
                                     remaining_seconds=runner.remaining_seconds(), counts=summary["counts"],
                                     duration_seconds=config.soak_seconds)
        runner.evidence.save("suite-summary.json", summary, required=True)
