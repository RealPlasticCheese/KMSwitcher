import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from kms_stress import acceptance
from kms_stress.__main__ import main


LIMITS = {"p95_ms": 7, "p99_ms": 8, "max_ms": 10, "max_gap_ms": 30}


def capture(window, samples=100):
    return {"clock": "shared acquisition counter", "window": window,
            "samples": [{"sequence": n, "sent_ns": n * 20_000_000, "slot": 2,
                         "observed": [{"slot": 2, "ns": n * 20_000_000 + 5_000_000}]}
                        for n in range(1, samples + 1)]}


class LatencyTests(unittest.TestCase):
    def test_tail_latency_and_gaps_are_independent_of_median_delivery(self):
        trace = capture("contention-keyboard")
        for sample in trace["samples"][-2:]:
            sample["observed"][0]["ns"] = sample["sent_ns"] + 25_000_000
        result = acceptance.latency_metrics("contention-keyboard", trace, LIMITS)
        self.assertEqual(result["measured"], dict(p95_ms=5, p99_ms=25, max_ms=25, max_gap_ms=40))
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["exceeded"], ["p99_ms", "max_ms", "max_gap_ms"])

    def test_loss_duplicates_wrong_destination_and_reordering_fail(self):
        trace = capture("baseline-pointer")
        trace["samples"][0]["observed"] = []
        trace["samples"][1]["observed"] *= 2
        trace["samples"][2]["observed"][0]["slot"] = 3
        trace["samples"][3]["observed"][0]["ns"] += 100_000_000
        result = acceptance.latency_metrics("baseline-pointer", trace, LIMITS)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["errors"], dict(missing=2, duplicate=1, wrong_slot=1, reordered=1))

    def test_require_complete_source_sequence_and_a_common_clock(self):
        trace = capture("baseline-keyboard")
        del trace["samples"][3]
        with self.assertRaisesRegex(ValueError, "source sequence"):
            acceptance.latency_metrics("baseline-keyboard", trace, LIMITS)
        trace = capture("baseline-keyboard")
        trace["samples"][0]["observed"][0]["ns"] = 1
        with self.assertRaisesRegex(ValueError, "timestamps"):
            acceptance.latency_metrics("baseline-keyboard", trace, LIMITS)
        with self.assertRaisesRegex(ValueError, "acquisition clock"):
            acceptance.latency_metrics("baseline-keyboard", {"samples": []}, LIMITS)
        with self.assertRaisesRegex(ValueError, "latency limit"):
            acceptance.latency_metrics("baseline-keyboard", capture("baseline-keyboard"), {**LIMITS, "p99_ms": float("inf")})

    def test_require_enough_samples_but_exclude_between_handoff_idle_from_gap(self):
        self.assertEqual(acceptance.latency_metrics("baseline-keyboard", capture("baseline-keyboard", 99), LIMITS)["status"], "INCONCLUSIVE")
        result = acceptance.latency_metrics("handover", capture("handover", 20), LIMITS)
        self.assertEqual(result["status"], "PASS")
        self.assertNotIn("max_gap_ms", result["measured"])


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        (self.directory / "log.txt").write_text("fixture observations", encoding="utf-8")

    def observed_plan(self, case):
        plan = acceptance.plan(case)
        plan["fixture"] = "Three PCs on Windows/Linux, spare S3 and identified S2 images"
        for item in plan["cases"].values():
            item["artifacts"] = ["log.txt"]
            for observation in item["observations"].values():
                observation.update(status="PASS", note="Verified in log.txt")
        return plan

    def test_requires_evidence_for_all_physical_cases_and_preserves_failure(self):
        plan = self.observed_plan("all")
        self.assertEqual(set(plan["cases"]), {"contention-latency", "radio-recovery", "desk-switching", "host-power",
                                             "usb-hotplug", "clipboard", "companion-uptime", "mixed-soak", "ota"})
        del plan["cases"]["desk-switching"]
        report = acceptance.evaluate(plan, self.directory)
        self.assertEqual(report["status"], "INCONCLUSIVE")
        self.assertEqual(report["cases"]["desk-switching"]["status"], "INCONCLUSIVE")
        self.assertEqual(report["cases"]["usb-hotplug"]["status"], "PASS")
        self.assertEqual(len(report["cases"]["usb-hotplug"]["artifacts"]["log.txt"]["sha256"]), 64)
        plan["cases"]["radio-recovery"]["observations"]["restart-guards"]["status"] = "FAIL"
        self.assertEqual(acceptance.evaluate(plan, self.directory)["status"], "FAIL")

    def test_latency_requires_all_windows_and_cannot_be_passed_by_notes_alone(self):
        plan = self.observed_plan("contention-latency")
        windows = plan["cases"]["contention-latency"]["latency"]
        for name in acceptance.WINDOWS:
            path = self.directory / f"{name}.json"
            path.write_text(json.dumps(capture(name)), encoding="utf-8")
            windows[name] = {"path": path.name, "limits_ms": LIMITS}
        self.assertEqual(acceptance.evaluate(plan, self.directory)["status"], "PASS")
        with self.assertRaisesRegex(ValueError, "window does not match"):
            acceptance.latency_metrics("contention-pointer", capture("baseline-pointer"), LIMITS)
        windows["handover"]["path"] = ""
        self.assertEqual(acceptance.evaluate(plan, self.directory)["status"], "INCONCLUSIVE")

    def test_mixed_soak_import_requires_complete_hardware_run_at_requested_rate(self):
        plan = self.observed_plan("mixed-soak")
        plan["cases"]["mixed-soak"]["runner_result"] = "result.json"
        run = {"hardware_test": True, "status": "PASS",
               "workload": {"preset": "mixed-soak", "receiver_slots": [2, 4]},
               "segments": [{"slot": 2, "status": "PASS", "profile": "mixed"},
                            {"slot": 4, "status": "PASS", "profile": "mixed"}]}
        for change, expected in (({}, "PASS"), ({"status": "LIMITED"}, "INCONCLUSIVE"),
                                 ({"segments": run["segments"][:1]}, "INCONCLUSIVE"), ({"status": "FAIL"}, "FAIL")):
            with self.subTest(change=change):
                (self.directory / "result.json").write_text(json.dumps({**run, **change}), encoding="utf-8")
                self.assertEqual(acceptance.evaluate(plan, self.directory)["status"], expected)
        with self.assertRaisesRegex(ValueError, "Mixed soak hardware run"):
            acceptance.mixed_soak_result({**run, "hardware_test": False})

    def test_artifacts_must_exist_and_stay_in_evidence_directory(self):
        plan = self.observed_plan("ota")
        plan["cases"]["ota"]["artifacts"] = ["missing.log"]
        self.assertEqual(acceptance.evaluate(plan, self.directory)["status"], "INCONCLUSIVE")
        plan["cases"]["ota"]["artifacts"] = ["../outside"]
        with self.assertRaisesRegex(ValueError, "inside the evidence directory"):
            acceptance.evaluate(plan, self.directory)

    def test_packaged_cli_uses_offline_acceptance_entry_without_hardware_lock(self):
        stdout = io.StringIO()
        with patch("sys.argv", ["kms-stress", "acceptance", "plan", "host-power"]), \
             patch.dict("sys.modules", {"kms_update.fixture_lock": None}), contextlib.redirect_stdout(stdout):
            self.assertEqual(main(), 0)
        self.assertEqual(json.loads(stdout.getvalue())["selection"], "host-power")
        evidence = self.directory / "plan.json"
        evidence.write_text(json.dumps(self.observed_plan("host-power")), encoding="utf-8")
        path = self.directory / "report.json"
        self.assertEqual(acceptance.main(["check", str(evidence), "--report", str(path)]), 0)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["status"], "PASS")
