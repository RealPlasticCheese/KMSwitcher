"""Ledger provenance, historical compatibility and verdict preservation."""
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from kms_stress.model import Config
from kms_stress.run_ledger import build_record, encoded, load_records, main, render_index, save_record
from kms_stress.runner import Evidence


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run = self.root / "20260101T010000Z-abcdef"
        self.run.mkdir()
        self.result = {"status": "PASS", "hardware_test": True, "cleanup": [],
                       "coverage": {"usb_only": True}, "workaround": True, "segments": [
            {"status": "PASS", "seconds": 14400, "rate": 30, "mode": "input", "profile": "stress", "slot": 0,
             "workaround": True, "generators": {"1": {"completed": 432018, "crc": 112458637}},
             "workaround_diagnostics": {"waited": 18418, "timeouts": 0, "crossings": 0}}]}
        self.write("result.json", self.result)
        self.write("profile.json", {"master": {"identity": "020000001001", "uart": "private-port"},
            "fixture_notes": "stale 1.5.0 / Temu", "generators": [
                {"identity": "020000001002", "enabled": True}, {"identity": "020000001003", "enabled": False}]})
        self.write("environment.json", {"app_version": "0.3.9"})
        self.write("isolation-final.json", {"fault": 0, "sources": {"020000001002": {"count": 432018, "crc": 112458637}}, "lines": ["not copied"]})
        capture = [{"kind": "uart", "line": "KMSISO INFO 1 020000001001 deadbeef 123456789abc idf=v6.1 usb=1.6.0 trace=1 workaround=1"},
                   {"kind": "uart", "line": "KMSGEN INFO 1 020000001002 54 23456789abcd trace=1"},
                   {"kind": "input", "line": "private input data"}]
        (self.run / "capture.jsonl").write_text("\n".join(json.dumps(e) for e in capture) + "\n", encoding="utf-8")
        (self.run / "report.html").write_text("saved HTML", encoding="utf-8")

    def write(self, name, value):
        (self.run / name).write_text(json.dumps(value), encoding="utf-8")

    def test_pass_metrics_and_observed_versions_survive_stale_profile(self):
        record = build_record(self.run)
        self.assertEqual(record["status"], "PASS")
        self.assertEqual(record["started_utc"], "2026-01-01T01:00:00+00:00")
        master = next(f for f in record["observed_firmware"] if f["role"] == "isolation-master")
        self.assertEqual(master["usb_host"], "1.6.0")
        self.assertEqual(record["configuration"]["fixture_notes_unverified"], "stale 1.5.0 / Temu")
        self.assertEqual(record["configuration"]["enabled_generator_identities"], ["020000001002"])
        segment = record["segments"][0]
        self.assertEqual(segment["generators"]["1"]["completed"], 432018)
        self.assertEqual(segment["workaround_diagnostics"]["waited"], 18418)
        self.assertEqual(record["snapshots"]["isolation-final.json"]["sources"]["020000001002"]["crc"], 112458637)
        self.assertNotIn("private input data", encoded(record))
        self.assertNotIn("private-port", encoded(record))
        self.assertNotIn("not copied", encoded(record))
        for name, info in record["archive"]["files"].items():
            self.assertEqual(info["sha256"], hashlib.sha256((self.run / name).read_bytes()).hexdigest())

    def test_deterministic_and_derived_files_excluded(self):
        first = build_record(self.run)
        self.write("ledger-record.json", first)
        (self.run / "ledger-error.txt").write_text("old error")
        self.assertEqual(first, build_record(self.run))

    def test_old_demo_campaign_and_failed_setup_remain_distinct(self):
        for status, hardware, segments, expected_kind in [
            ("DEMO PASS", False, [], "demo"), ("CANCELLED", True, [], "hardware"),
            ("INCONCLUSIVE", True, [{"mode": "input", "errors": ["invalid command"], "run": 0}], "usb-isolation"),
            ("FAIL", True, [{"case_id": "abcd", "usb_classification": {"cause": "unconfirmed"},
                             "recipe": {"case": {"activity": "idle", "idle_seconds": 30, "first_source": 2}}}], "hardware"),
        ]:
            with self.subTest(status=status):
                self.write("result.json", {"status": status, "hardware_test": hardware, "segments": segments})
                record = build_record(self.run)
                self.assertEqual((record["status"], record["kind"]), (status, expected_kind))
                self.assertIsNone(record["requested_workaround"])
                self.assertEqual(record["segments"], segments)

    def test_missing_optional_metadata_is_unknown(self):
        for path in self.run.iterdir():
            if path.name != "result.json": path.unlink()
        record = build_record(self.run)
        self.assertEqual(record["observed_firmware"], [])
        self.assertEqual(record["environment"], {})

    def test_active_corrupt_and_invalid_directories_rejected(self):
        (self.run / "result.json").unlink()
        with self.assertRaises(ValueError): build_record(self.run)
        (self.run / "result.json").write_text("{")
        with self.assertRaises(ValueError): build_record(self.run)
        with self.assertRaises(ValueError): build_record(self.root)

    def test_idempotent_import_conflict_never_overwrites(self):
        output = self.root / "ledger"
        args = ["import", str(self.run), "--output", str(output)]
        self.assertEqual(main(args), 0)
        self.assertEqual(main(args), 0)
        target = output / "records" / (self.run.name + ".json")
        before = target.read_bytes()
        self.result["status"] = "FAIL"
        self.write("result.json", self.result)
        with self.assertRaises(SystemExit): main(args)
        self.assertEqual(target.read_bytes(), before)

    def test_recursive_import_skips_unfinished_folders(self):
        (self.root / "20260101T010001Z-aaaaaa").mkdir()
        ledger = self.root / "ledger"
        self.assertEqual(main(["import", str(self.root), "--output", str(ledger), "--recursive"]), 0)
        self.assertEqual(len(list((ledger / "records").glob("*.json"))), 1)

    def test_relocated_identical_archive_retains_original_location(self):
        ledger = self.root / "ledger"
        main(["import", str(self.run), "--output", str(ledger)])
        copy = self.root / "copy" / self.run.name
        shutil.copytree(self.run, copy)
        main(["import", str(copy), "--output", str(ledger)])
        saved = json.loads((ledger / "records" / (self.run.name + ".json")).read_text())
        self.assertEqual(saved["archive"]["original_path"], str(self.run.resolve()))

    def test_annotations_index_and_staleness(self):
        ledger = self.root / "ledger"
        save_record(build_record(self.run), ledger / "records" / (self.run.name + ".json"))
        annotation = {self.run.name: {"topology": "EDO hub", "source": "owner report", "note": "a | b\n<c>"}}
        (ledger / "annotations.json").write_text(json.dumps(annotation))
        index = render_index(ledger)
        self.assertIn("EDO hub", index)
        self.assertIn("a \\| b &lt;c&gt;", index)
        self.assertIn("432018", index)
        self.assertEqual(main(["index", str(ledger)]), 0)
        self.assertEqual(main(["check", str(ledger)]), 0)
        (ledger / "INDEX.md").write_text("stale")
        with self.assertRaises(SystemExit): main(["check", str(ledger)])
        annotation["absent"] = {}
        (ledger / "annotations.json").write_text(json.dumps(annotation))
        with self.assertRaises(ValueError): render_index(ledger)

    def test_invalid_fingerprint_rejected(self):
        ledger = self.root / "ledger"
        record = build_record(self.run)
        record["archive"]["files"]["result.json"]["sha256"] = "not a digest"
        save_record(record, ledger / "records" / (self.run.name + ".json"))
        with self.assertRaises(ValueError): load_records(ledger)

    def test_finish_creates_record_after_capture_and_html_close(self):
        evidence = Evidence(self.root, Config())
        result = {"status": "CANCELLED", "hardware_test": True, "segments": []}
        evidence.finish(result)
        record = json.loads((evidence.path / "ledger-record.json").read_text())
        self.assertEqual(record["status"], "CANCELLED")
        self.assertIn("report.html", record["archive"]["files"])
        self.assertTrue(evidence.file.closed)

    def test_export_failure_preserves_verdict_and_reports_error(self):
        evidence = Evidence(self.root, Config())
        result = {"status": "PASS", "hardware_test": True, "segments": []}
        with patch("kms_stress.run_ledger.build_record", side_effect=ValueError("broken metadata")):
            evidence.finish(result)
        self.assertEqual(json.loads((evidence.path / "result.json").read_text()), result)
        self.assertIn("broken metadata", (evidence.path / "ledger-error.txt").read_text())


if __name__ == "__main__": unittest.main()
