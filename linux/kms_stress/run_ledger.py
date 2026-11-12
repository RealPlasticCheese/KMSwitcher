"""Export small, reproducible run records without opening hardware or changing results."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re


RUN_ID = re.compile(r"\d{8}T\d{6}Z-[0-9a-f]{6}")
STATUSES = {"PASS", "FAIL", "LIMITED", "INCONCLUSIVE", "CANCELLED", "DEMO PASS", "DEMO FAIL"}
SEGMENT_FIELDS = (
    "status", "slot", "rate", "seconds", "profile", "mode", "workaround", "seed",
    "run", "sources", "reports", "errors", "elapsed_seconds", "generation_seconds",
    "actual_reports_per_second", "generators", "inactive_generators", "comparisons",
    "workaround_diagnostics", "case_id", "attempt", "usb_classification",
    "usb_evidence_errors", "primary_failure", "first_divergence",
    "recipe", "observed_faults", "keyboard_checkpoints", "diagnosis",
    "test", "test_id", "cycle", "duration_limited",
    "injections", "overlap_evidence", "requested_reports_per_second",
)
SNAPSHOT_FIELDS = ("run", "boot", "at", "fault", "watching", "frozen", "expected", "devices", "sources", "workaround")
DERIVED_FILES = {"ledger-record.json", "ledger-error.txt"}


def read_json(path, default=None):
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def select(data, keys):
    return {key: data[key] for key in keys if key in data}


def encoded(value):
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True, allow_nan=False) + "\n"


def file_digest(path):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(chunk)
            digest.update(chunk)
    return {"bytes": size, "sha256": digest.hexdigest()}


def firmware_identity(line):
    fields = line.split()
    if len(fields) >= 8 and fields[:3] == ["KMSISO", "INFO", "1"]:
        return {"role": "isolation-master", "identity": fields[3], "revision": fields[5],
                "idf": fields[6].removeprefix("idf="), "usb_host": fields[7].removeprefix("usb=")}
    if len(fields) >= 6 and fields[:3] == ["KMSGEN", "INFO", "1"]:
        return {"role": "generator", "identity": fields[3], "revision": fields[5]}
    return None


def capture_firmware(path):
    """Stream the capture; retain identity replies, never arbitrary UART/input data."""
    found = []
    if path.exists():
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                event = json.loads(line)
                if event.get("kind") != "uart":
                    continue
                identity = firmware_identity(event.get("line", ""))
                if identity and identity not in found:
                    found.append(identity)
    return found


def build_record(directory):
    directory = Path(directory)
    run_id = directory.name
    if not RUN_ID.fullmatch(run_id):
        raise ValueError(f"Not a runner result directory: {run_id}")
    result = read_json(directory / "result.json")
    if not isinstance(result, dict) or result.get("status") not in STATUSES:
        raise ValueError(f"{run_id}: missing/invalid final result.json; active runs cannot be imported")
    profile = read_json(directory / "profile.json", {})
    environment = read_json(directory / "environment.json", {})
    segments = result.get("segments", [])
    isolation = result.get("coverage", {}).get("usb_only") is True or any("mode" in s for s in segments)
    kind = "demo" if result.get("hardware_test") is False or result["status"].startswith("DEMO") else (
        "usb-isolation" if isolation else "hardware" if result.get("hardware_test") is True else "unknown")
    identities = capture_firmware(directory / "capture.jsonl")
    for segment in segments:
        reply = segment.get("firmware")
        if isinstance(reply, list):
            identity = firmware_identity(" ".join(reply))
            if identity and identity not in identities:
                identities.append(identity)
    inventory = read_json(directory / "isolation-inventory.json", {})
    if inventory.get("info"):
        identity = firmware_identity(" ".join(inventory["info"]))
        if identity and identity not in identities:
            identities.append(identity)
    master_inventory = read_json(directory / "master-inventory.json", {})
    for line in master_inventory.get("lines", []):
        match = re.search(r"Git: ([0-9a-f]{40}); application: (\w+)", line)
        if match:
            identities.append({"role": "controller", "revision": match[1], "application": match[2]})
    snapshots = {}
    for name in ("isolation-final.json", "isolation-fault.json"):
        snapshot = read_json(directory / name)
        if snapshot is not None:
            snapshots[name] = select(snapshot, SNAPSHOT_FIELDS)
    traces = {}
    for path in sorted(directory.glob("generator-trace-*.json")):
        traces[path.name] = read_json(path).get("summary", {})
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Archive contains a symlink: {path}")
        if path.is_file() and path.name not in DERIVED_FILES:
            files[path.relative_to(directory).as_posix()] = file_digest(path)
    return {
        "schema_version": 1, "run_id": run_id,
        "started_utc": datetime.strptime(run_id[:16], "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc).isoformat(),
        "kind": kind, "status": result["status"], "hardware_test": result.get("hardware_test"),
        "message": result.get("message", ""), "cleanup": result.get("cleanup", []),
        "coverage": result.get("coverage", {}), "workload_text": result.get("workload_text"),
        "suite": result.get("suite"),
        "requested_workaround": result.get("workaround"),
        "configuration": {
            "preset": profile.get("preset"), "seed": profile.get("seed"),
            "master_identity": profile.get("master", {}).get("identity"),
            "enabled_generator_identities": [d.get("identity") for d in profile.get("generators", []) if d.get("enabled", True)],
            "enabled_receiver_identities": [select(d, ("identity", "slot")) for d in profile.get("receivers", []) if d.get("enabled", True)],
            "fixture_notes_unverified": profile.get("fixture_notes", ""),
        },
        "environment": select(environment, ("app_version", "python", "platform")),
        "observed_firmware": sorted(identities, key=encoded),
        "segments": [select(segment, SEGMENT_FIELDS) for segment in segments],
        "snapshots": snapshots, "generator_trace_summaries": traces,
        "archive": {"original_path": str(directory.resolve()), "files": files},
    }


def save_record(record, path):
    """Identical imports are harmless; a changed archive must be reviewed explicitly."""
    text = encoded(record)
    if path.exists():
        if not same_record(read_json(path), record):
            raise ValueError(f"Conflicting record: {path}; refusing to overwrite evidence")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def same_record(left, right):
    # Moving a byte-identical archive is not new evidence. Keep the original
    # recorded location rather than rewriting provenance on a repeated import.
    left = {**left, "archive": {**left["archive"], "original_path": None}}
    right = {**right, "archive": {**right["archive"], "original_path": None}}
    return left == right


def cell(value):
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ").replace("<", "&lt;").replace(">", "&gt;")


def load_records(ledger):
    records = []
    for path in sorted((ledger / "records").glob("*.json")):
        record = read_json(path)
        if (record.get("schema_version") != 1 or not RUN_ID.fullmatch(record.get("run_id", ""))
                or path.stem != record["run_id"] or record.get("status") not in STATUSES):
            raise ValueError(f"Invalid ledger record: {path}")
        if "result.json" not in record["archive"]["files"]:
            raise ValueError(f"Missing result fingerprint: {path}")
        for name, info in record["archive"]["files"].items():
            if (name.startswith("/") or ".." in Path(name).parts or "\\" in name
                    or not re.fullmatch(r"[0-9a-f]{64}", info.get("sha256", ""))
                    or type(info.get("bytes")) is not int or info["bytes"] < 0):
                raise ValueError(f"Invalid archive fingerprint: {path}: {name}")
        records.append(record)
    return records


def render_index(ledger):
    records = load_records(ledger)
    annotations = read_json(ledger / "annotations.json", {})
    unknown = set(annotations) - {r["run_id"] for r in records}
    if unknown:
        raise ValueError(f"Annotations reference absent runs: {sorted(unknown)}")
    counts = Counter(r["status"] for r in records)
    lines = ["# Recorded test runs", "", "Generated by `python -m kms_stress.run_ledger index docs/test-runs`. See [README](README.md).", "",
             f"{len(records)} finalized results: " + ", ".join(f"{counts[s]} {s}" for s in sorted(counts)) + ".", "",
             "Setup failures, cancellations and demos are retained. PASS describes the saved verdict, not a proven fix.", "",
             "| Run (UTC) | Kind / result | Observed master / USB host | Workload | Generator completions | Physical setup / notes |",
             "| --- | --- | --- | --- | --- | --- |"]
    for record in records:
        run_id = record["run_id"]
        firmware = [f for f in record["observed_firmware"] if f["role"] in ("isolation-master", "controller")]
        versions = "; ".join(f.get("revision", "unknown") + (" / " + f["usb_host"] if "usb_host" in f else "") for f in firmware) or "unknown"
        segments = record["segments"]
        workload = "; ".join(dict.fromkeys(f"{s.get('seconds', '?')}s @ {s.get('rate', '?')}/s; {s.get('mode', s.get('profile', '?'))}" +
                            (f"; guard={str(s['workaround']).lower()}" if "workaround" in s else "") for s in record["segments"])) or "not started"
        if len(segments) > 1:
            workload = f"{len(segments)} segments; see record"
        completions = ", ".join(f"G{source}={stats.get('completed', '?')}" for source, stats in
                                 (segments[-1].get("generators", {}) if segments else {}).items()) or "not recorded"
        if len(segments) > 1 and completions != "not recorded":
            completions = "last segment: " + completions
        annotation = annotations.get(run_id, {})
        notes = annotation.get("topology", "unknown; saved profile notes are unverified")
        if annotation.get("note"):
            notes += "; " + annotation["note"]
        lines.append("| " + " | ".join((f"[{run_id}](records/{run_id}.json)", cell(record["kind"] + " / " + record["status"]),
                     cell(versions), cell(workload), cell(completions), cell(notes))) + " |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    importing = sub.add_parser("import", help="Import a finalized capture directory (no hardware access)")
    importing.add_argument("source", type=Path)
    importing.add_argument("--output", required=True, type=Path)
    importing.add_argument("--recursive", action="store_true", help="Find result.json folders below source; skip unfinished folders")
    for name in ("index", "check"):
        sub.add_parser(name).add_argument("ledger", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "import":
            directories = sorted({p.parent for p in args.source.rglob("result.json")}) if args.recursive else [args.source]
            if not directories:
                raise ValueError("No finalized result.json files found")
            # Build and check every record before writing any from this import.
            records = [build_record(path) for path in directories]
            targets = {}
            for record in records:
                target = args.output / "records" / (record["run_id"] + ".json")
                prior = read_json(target, targets.get(target))
                if prior is not None and not same_record(prior, record):
                    raise ValueError(f"Conflicting record: {target}; archive contents differ")
                targets[target] = record
            for path, record in targets.items():
                save_record(record, path)
            print(f"Imported {len(targets)} finalized records; run the index command after reviewing annotations.")
        else:
            index = render_index(args.ledger)
            path = args.ledger / "INDEX.md"
            if args.command == "check":
                if not path.exists() or path.read_text(encoding="utf-8") != index:
                    raise ValueError("Ledger index is stale; run the index command")
                print(f"Verified {len(load_records(args.ledger))} records and index")
            else:
                path.write_text(index, encoding="utf-8", newline="\n")
        return 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, str(error) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
