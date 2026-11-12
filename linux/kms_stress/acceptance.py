"""Check physical acceptance evidence independently of the test-mode runner."""
import argparse
import json
import math
from pathlib import Path

from . import __version__
from .acceptance_cases import CASES
from .run_ledger import encoded, file_digest


MAX_CAPTURE_BYTES = 32 * 1024 * 1024
WINDOWS = ("baseline-keyboard", "baseline-pointer", "contention-keyboard", "contention-pointer", "handover")


def latency_metrics(window, trace, limits):
    """Measure paired input/host edges acquired on one clock."""
    minimum = 20 if window == "handover" else 100
    required = ("p95_ms", "p99_ms", "max_ms")
    if window != "handover":
        required += ("max_gap_ms",)
    if not isinstance(limits, dict) or any(
            type(limits.get(key)) not in (int, float) or not 0 < limits[key] <= 3600000 for key in required):
        raise ValueError(f"{window}: set a finite positive latency limit for each metric")
    if not isinstance(trace, dict) or not isinstance(trace.get("clock"), str) or not trace["clock"].strip():
        raise ValueError(f"{window}: identify the common acquisition clock")
    if trace.get("window") != window:
        raise ValueError(f"{window}: capture window does not match")
    samples = trace.get("samples")
    if not isinstance(samples, list) or len(samples) > 100_000:
        raise ValueError(f"{window}: expected at most 100000 samples")
    delays, gaps = [], []
    missing = duplicate = wrong_slot = reordered = 0
    last_sequence = 0
    last_sent = -1
    last_host = None
    for sample in samples:
        if not isinstance(sample, dict):
            raise ValueError(f"{window}: malformed sample")
        sequence, sent, slot, observed = (sample.get(key) for key in ("sequence", "sent_ns", "slot", "observed"))
        if (type(sequence) is not int or sequence <= last_sequence or type(sent) is not int or
                not 0 <= sent < 2**64 or sent <= last_sent or type(slot) is not int or not 1 <= slot <= 7 or
                not isinstance(observed, list)):
            raise ValueError(f"{window}: invalid slot or unordered source samples")
        if sequence != last_sequence + 1:
            raise ValueError(f"{window}: source sequence is incomplete")
        last_sequence, last_sent = sequence, sent
        for event in observed:
            if (not isinstance(event, dict) or type(event.get("slot")) is not int or not 1 <= event["slot"] <= 7 or
                    type(event.get("ns")) is not int or not sent <= event["ns"] < 2**64):
                raise ValueError(f"{window}: invalid observation or incompatible timestamps")
        matching = [event["ns"] for event in observed if event["slot"] == slot]
        wrong_slot += len(observed) - len(matching)
        if not matching:
            missing += 1
            continue
        duplicate += len(matching) - 1
        received = min(matching)
        if last_host is not None:
            if received < last_host:
                reordered += 1
            else:
                gaps.append(received - last_host)
        last_host = received
        delays.append(received - sent)
    delays.sort()
    measured = {}
    if delays:
        measured = {"p95_ms": delays[math.ceil(0.95 * len(delays)) - 1] / 1e6,
                    "p99_ms": delays[math.ceil(0.99 * len(delays)) - 1] / 1e6,
                    "max_ms": delays[-1] / 1e6}
    if gaps and window != "handover":
        measured["max_gap_ms"] = max(gaps) / 1e6
    exceeded = [key for key in required if key in measured and measured[key] > limits[key]]
    errors = {"missing": missing, "duplicate": duplicate, "wrong_slot": wrong_slot, "reordered": reordered}
    status = "FAIL" if any(errors.values()) or exceeded else "PASS" if len(samples) >= minimum else "INCONCLUSIVE"
    return dict(status=status, clock=trace["clock"], samples=len(samples), minimum_samples=minimum,
                measured=measured, limits={key: limits[key] for key in required}, exceeded=exceeded, errors=errors)


def plan(selection):
    cases = CASES if selection == "all" else {selection: CASES[selection]}
    result = {"version": 1, "selection": selection, "fixture": "", "cases": {}}
    for name, case in cases.items():
        item = dict(steps=case["steps"], criteria=case["checks"], artifacts=[],
                    observations={key: {"status": "", "note": ""} for key in case["checks"]})
        if name == "contention-latency":
            item["latency"] = {
                window: {"path": "", "limits_ms": dict(p95_ms=None, p99_ms=None, max_ms=None,
                                                      **({} if window == "handover" else {"max_gap_ms": None}))}
                for window in WINDOWS
            }
        if name == "mixed-soak":
            item["runner_result"] = ""
        result["cases"][name] = item
    return result


def read_json(path):
    with path.open("rb") as stream:
        data = stream.read(MAX_CAPTURE_BYTES + 1)
    if len(data) > MAX_CAPTURE_BYTES:
        raise ValueError(f"{path.name}: acceptance JSON exceeds {MAX_CAPTURE_BYTES // (1024 * 1024)} MiB")
    return json.loads(data)


def evidence_path(directory, name):
    if not isinstance(name, str) or not name.strip() or Path(name).is_absolute():
        raise ValueError("Evidence references must be relative file paths")
    path = (directory / name).resolve()
    if not path.is_relative_to(directory):
        raise ValueError("Evidence references must stay inside the evidence directory")
    return path


def mixed_soak_result(result):
    if (not isinstance(result, dict) or result.get("hardware_test") is not True or
            result.get("workload", {}).get("preset") != "mixed-soak"):
        raise ValueError("Attach result.json from a Mixed soak hardware run")
    slots = result["workload"].get("receiver_slots", [])
    segments = result.get("segments", [])
    statuses = [item["status"] for item in segments]
    if result.get("status") == "FAIL" or "FAIL" in statuses:
        return "FAIL"
    if (result.get("status") == "PASS" and slots and [item["slot"] for item in segments] == slots and
            all(item["status"] == "PASS" and item.get("profile") == "mixed" and not item.get("partial")
                for item in segments)):
        return "PASS"
    return "INCONCLUSIVE"


def evaluate(document, directory):
    if not isinstance(document, dict) or document.get("version") != 1:
        raise ValueError("Expected a version 1 acceptance plan")
    selection = document.get("selection")
    if selection != "all" and (not isinstance(selection, str) or selection not in CASES):
        raise ValueError("Unknown acceptance selection")
    fixture = document.get("fixture")
    if not isinstance(fixture, str) or not fixture.strip():
        raise ValueError("Describe the physical fixture and firmware/app builds")
    directory = Path(directory).resolve()
    expected = plan(selection)["cases"]
    supplied = document.get("cases")
    if not isinstance(supplied, dict) or supplied.keys() - expected.keys():
        raise ValueError("Case evidence must match the selected plan")
    report = {"version": 1, "app_version": __version__, "selection": selection, "fixture": fixture, "cases": {}}
    for name, case in expected.items():
        item = supplied.get(name, {})
        if not isinstance(item, dict):
            raise ValueError(f"{name}: expected case evidence")
        observations = item.get("observations", {})
        if not isinstance(observations, dict) or observations.keys() - case["criteria"].keys():
            raise ValueError(f"{name}: unexpected observation")
        details = dict(observations={}, artifacts={}, missing=[])
        statuses = []
        for key in case["criteria"]:
            observed = observations.get(key, {})
            if not isinstance(observed, dict):
                raise ValueError(f"{name}/{key}: expected status and note")
            status, note = observed.get("status", ""), observed.get("note", "")
            if status not in ("", "PASS", "FAIL", "INCONCLUSIVE") or not isinstance(note, str):
                raise ValueError(f"{name}/{key}: invalid status or note")
            if not note.strip() or not status:
                details["missing"].append(key)
                status = "FAIL" if status == "FAIL" else "INCONCLUSIVE"
            details["observations"][key] = dict(status=status, note=note)
            statuses.append(status)
        artifacts = item.get("artifacts", [])
        if not isinstance(artifacts, list) or len(artifacts) > 32:
            raise ValueError(f"{name}: provide up to 32 evidence files")
        if not artifacts:
            details["missing"].append("supporting artifacts")
        for artifact in artifacts:
            path = evidence_path(directory, artifact)
            if path.is_file() and path.stat().st_size:
                details["artifacts"][artifact] = file_digest(path)
            else:
                details["missing"].append(artifact)
        if name == "contention-latency":
            windows = item.get("latency", {})
            if not isinstance(windows, dict) or windows.keys() - set(WINDOWS):
                raise ValueError("Unexpected latency window")
            details["latency"] = {}
            for window in WINDOWS:
                measurement = windows.get(window, {})
                if not isinstance(measurement, dict):
                    raise ValueError(f"{window}: expected path and limits_ms")
                path_name = measurement.get("path", "")
                if not path_name:
                    details["missing"].append(window)
                    continue
                path = evidence_path(directory, path_name)
                if not path.is_file():
                    details["missing"].append(path_name)
                    continue
                metrics = latency_metrics(window, read_json(path), measurement.get("limits_ms"))
                details["latency"][window] = metrics
                details["artifacts"][path_name] = file_digest(path)
                statuses.append(metrics["status"])
        if name == "mixed-soak":
            path_name = item.get("runner_result", "")
            if path_name and (path := evidence_path(directory, path_name)).is_file():
                details["runner_status"] = mixed_soak_result(read_json(path))
                details["artifacts"][path_name] = file_digest(path)
                statuses.append(details["runner_status"])
            else:
                details["missing"].append("Mixed soak result.json")
        if details["missing"]:
            statuses.append("INCONCLUSIVE")
        details["status"] = "FAIL" if "FAIL" in statuses else "INCONCLUSIVE" if "INCONCLUSIVE" in statuses else "PASS"
        report["cases"][name] = details
    statuses = {item["status"] for item in report["cases"].values()}
    report["status"] = "FAIL" if "FAIL" in statuses else "INCONCLUSIVE" if "INCONCLUSIVE" in statuses else "PASS"
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    prepare = commands.add_parser("plan", help="Print a physical procedure and evidence template")
    prepare.add_argument("case", nargs="?", default="all", choices=("all", *CASES))
    check = commands.add_parser("check", help="Evaluate observations and measured evidence")
    check.add_argument("evidence", type=Path)
    check.add_argument("--report", type=Path, help="Write the result to a new JSON file")
    args = parser.parse_args(argv)
    try:
        if args.action == "plan":
            print(encoded(plan(args.case)), end="")
            return 0
        report = evaluate(read_json(args.evidence), args.evidence.parent)
        output = encoded(report)
        if args.report:
            with args.report.open("x", encoding="utf-8") as stream:
                stream.write(output)
        else:
            print(output, end="")
        return 0 if report["status"] == "PASS" else 1
    except (KeyError, TypeError, ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
