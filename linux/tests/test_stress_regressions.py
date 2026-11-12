"""Deterministic runner regressions with fake device adapters."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from kms_stress.model import Config, Device, verify_overlap
from kms_stress.diagnostics import GeneratorStopError, device_fault_errors
from kms_stress.runner import Cancelled, Evidence, Runner, diagnose
from kms_stress.web import Controller
from test_runner_integration import Clock, Collector, Fixture, Master, Source


def config(directory):
    return Config(master=Device("112233445566", uart="master"),
                  generators=[Device("AABBCCDDEE01", uart="a"), Device("AABBCCDDEE02", uart="b")],
                  receivers=[Device("AABBCCDDEE06", slot=6)], output=directory)


class PendingSource(Source):
    def __init__(self, fixture, source):
        super().__init__(fixture, source)
        self.stats_requests = 0

    def request(self, command, prefix=None, timeout=3):
        if command == "bench stop":
            self.running = False
            return ["KMSGEN STOPPING"]
        if command == "bench stats":
            self.stats_requests += 1
            self.fixture.runner.evidence.log("uart", {"port": "master", "line":
                "W (38246) diagnostics: Slot 6: USB control deadline (detail=1426129408, idle=0ms)"})
            return [f"KMSGEN STATS {self.fixture.run} {self.source} 840 840 {self.count} 0 {self.crc:08x} {int(self.running)} 1"]
        return super().request(command, prefix, timeout)

    def close(self): pass


class StressRegressionTests(unittest.TestCase):
    def overlap_coverage(self, preset, configured_sources, verified_sources, status, simultaneous=True):
        with tempfile.TemporaryDirectory() as directory:
            c = config(directory); c.preset = preset
            c.generators = c.generators[:configured_sources]
            runner = Runner(c)
            item = dict(slot=6, rate=2, seconds=2, profile="overlap", status=status,
                        errors=[] if status == "PASS" else ["Simulated overlap mismatch"],
                        generators={str(s): {"completed": 5} for s in range(1, verified_sources + 1)},
                        overlap_evidence={"key": simultaneous, "button": simultaneous})
            def timed_suite(r): r.completed_segments.append({**item, "test": "overlap"})
            with patch.object(runner, "preflight"), patch.object(runner, "segment", return_value=item), \
                 patch("kms_stress.suite.run", side_effect=timed_suite):
                result, _ = runner.run()
            return result["coverage"]

    def test_overlap_coverage_requires_two_verified_sources_and_a_passing_case(self):
        for preset in ("overlap", "all-tests"):
            for sources in (1, 2):
                for status in ("PASS", "FAIL"):
                    with self.subTest(preset=preset, sources=sources, status=status):
                        coverage = self.overlap_coverage(preset, sources, sources, status)
                        self.assertEqual(coverage["overlap_pattern"], status == "PASS")
                        self.assertEqual(coverage["shared_key_overlap"], sources == 2 and status == "PASS")

    def test_enabled_second_generator_alone_does_not_establish_shared_input_coverage(self):
        coverage = self.overlap_coverage("all-tests", 2, 1, "PASS")
        self.assertTrue(coverage["overlap_pattern"])
        self.assertFalse(coverage["shared_key_overlap"])

    def test_two_verified_sources_without_simultaneous_holds_do_not_establish_shared_input(self):
        coverage = self.overlap_coverage("overlap", 2, 2, "PASS", simultaneous=False)
        self.assertFalse(coverage["shared_key_overlap"])

    def test_synthetic_startup_timeouts_cannot_pass_matching_input(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/receiver-startup.json").read_text())
        self.assertEqual([c["segment"] for c in fixture["cases"] if c["reported_status"] == "PASS"], [2, 3, 5])
        for case in fixture["cases"]:
            with self.subTest(segment=case["segment"]):
                self.assertIn("Device fault: acknowledgement timeout",
                              device_fault_errors(case["observed_faults"], case["traces"]))
        for case in (fixture["cases"][2], fixture["cases"][4]):
            self.assertEqual(len({tuple(v) for v in case["keyboard_checkpoints"].values()}), 1)

    def test_synthetic_early_freeze_is_not_an_overlap_merge_reference(self):
        fixture = json.loads((Path(__file__).parent / "fixtures/receiver-startup.json").read_text())
        case = fixture["cases"][3]
        result = {"errors": [], "run": case["run"]}
        verify_overlap(result, case["traces"]["master"],
                       {int(s): x["completed"] for s, x in case["generators"].items()})
        self.assertTrue(result["overlap_reference_incomplete"])
        self.assertEqual(result["errors"], ["Shared-key reference needs a complete master ingress trace"])
        self.assertNotIn("expected_keyboard", result)

    def test_fault_window_ignores_old_fault_but_sees_a_post_arm_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            line = "W (12) diagnostics: Slot 6: acknowledgement timeout (detail=0, idle=0ms)"
            with patch("kms_stress.runner.time.monotonic_ns", return_value=100):
                evidence.log("uart", {"port": "master", "line": line})
            armed = evidence.fault_snapshot()
            self.assertEqual(armed, evidence.fault_snapshot())
            with patch("kms_stress.runner.time.monotonic_ns", return_value=200):
                evidence.log("uart", {"port": "master", "line": line})
            self.assertNotEqual(armed, evidence.fault_snapshot())
            self.assertNotIn("repeats", armed[0])
            self.assertEqual(evidence.fault_snapshot()[0]["host_ns"], 100)
            self.assertEqual(evidence.fault_snapshot()[0]["repeats"], 2)
            evidence.finish({"status": "FAIL"})

    def test_missing_final_statistics_alone_does_not_prove_release_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            runner.evidence = Evidence(directory, runner.config)
            runner.active_segment = dict(slot=6, rate=30, seconds=60, profile="stress", run=42)
            error = GeneratorStopError({}, ["Generator 1: UART unavailable"])
            partial = runner.interrupted_segment(error, {})
            runner.evidence.finish({"status": partial["status"], "segments": [partial]})
            self.assertEqual(partial["status"], "INCONCLUSIVE")

    def test_route_selection_and_usb_recovery_do_not_mask_initial_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            for name in ("computer selected", "USB host reconnect", "USB host recovered", "connection recovered"):
                evidence.log("uart", {"port": "master", "line": f"I (10) diagnostics: Slot 6: {name} (detail=0, idle=0ms)"})
            evidence.log("uart", {"port": "master", "line": "W (12) diagnostics: Slot 6: USB control deadline (detail=0, idle=0ms)"})
            evidence.finish({"status": "FAIL"})
            self.assertEqual([item["event"] for item in evidence.faults], ["USB control deadline"])

    def test_cancelled_run_retains_active_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            def cancel():
                runner.active_segment = dict(slot=6, rate=30, seconds=60, profile="stress", run=42)
                raise Cancelled("Stopped by operator")
            with patch.object(runner, "preflight", side_effect=cancel): result, _ = runner.run()
            self.assertEqual(result["status"], "CANCELLED")
            self.assertEqual(len(result["segments"]), 1)
            self.assertTrue(result["segments"][0]["partial"])
            self.assertEqual(result["segments"][0]["status"], "CANCELLED")

    def test_early_failure_rate_uses_generation_elapsed_not_planned_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            fixture = Fixture(runner, True)
            runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
            runner.master_verified = True
            runner.evidence = Evidence(directory, runner.config)
            with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                 patch("kms_stress.runner.time.monotonic", new=Clock()):
                result = runner.segment(1, 6, 10, 10, "everyday")
            runner.evidence.finish({"status": result["status"], "segments": [result]})
            self.assertLess(result["elapsed_seconds"], result["seconds"])
            for source in (1, 2):
                elapsed = result["generation_seconds"][source]
                self.assertAlmostEqual(result["actual_reports_per_second"][source],
                                       result["generators"][source]["completed"] / elapsed, places=2)
                self.assertGreater(result["actual_reports_per_second"][source], 1.0)

    def stalled_run(self, directory):
        runner = Runner(config(directory))
        runner.config.seconds = 60
        fixture = Fixture(runner)
        fixture.sources = [PendingSource(fixture, 1), PendingSource(fixture, 2)]
        runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
        runner.master.close = lambda: None
        runner.capture.close = lambda: None
        runner.master_verified = True
        clock = Clock()
        with patch.object(runner, "preflight"), patch.object(runner, "wait"), \
             patch.object(runner.cancel, "wait"), patch("kms_stress.runner.time.sleep"), \
             patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
             patch("kms_stress.runner.time.monotonic", new=clock):
            result, path = runner.run()
        return result, path, fixture

    def test_stalled_workload_fails_before_planned_end_and_keeps_partial_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            result, path, fixture = self.stalled_run(directory)
            self.assertEqual(result["status"], "FAIL", result["message"])
            self.assertEqual(len(result["segments"]), 1)
            segment = result["segments"][0]
            self.assertTrue(segment["partial"])
            self.assertLess(segment["elapsed_seconds"], 10)
            self.assertEqual(set(map(int, segment["generators"])), {1, 2})
            self.assertIn("USB control deadline", segment["primary_failure"])
            self.assertTrue(segment["shutdown_errors"])
            saved = json.loads((path / "result.json").read_text())
            self.assertEqual(saved["segments"][0]["run"], fixture.run)

    def test_release_timeout_collects_both_affected_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            fixture = Fixture(runner)
            fixture.sources = [PendingSource(fixture, 1), PendingSource(fixture, 2)]
            runner.generators = fixture.sources
            runner.evidence = Evidence(directory, runner.config)
            try:
                with patch("kms_stress.runner.time.monotonic", new=Clock()), patch("kms_stress.runner.time.sleep"):
                    with self.assertRaises(RuntimeError) as raised:
                        runner.stop_generators()
                self.assertTrue(all(source.stats_requests for source in fixture.sources))
                self.assertIn("Generator 1", str(raised.exception))
                self.assertIn("Generator 2", str(raised.exception))
                self.assertIn("pending", str(raised.exception))
            finally:
                runner.evidence.finish({"status": "INCONCLUSIVE"})

    def test_recovery_notification_is_not_counted_as_fault(self):
        findings = diagnose({"master": {"overwritten": 868, "records": [
            {"stage": 12, "a": 5, "b": 1, "c": 0, "micros": 100},
            {"stage": 12, "a": 14, "b": 1, "c": 256, "micros": 101},
            {"stage": 12, "a": 15, "b": 1, "c": 40, "micros": 102}]}}, [])
        text = "\n".join(findings)
        self.assertIn("transmit queue overflow", text)
        self.assertIn("replay dropped", text)
        self.assertIn("recovery notification", text)
        self.assertNotIn("3 fault records", text)
        self.assertIn("868 older records", text)

    def test_routing_mismatch_after_recovery_does_not_claim_decoding_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            fixture = Fixture(runner)
            runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
            runner.master_verified = True
            runner.evidence = Evidence(directory, runner.config)
            def recovered_trace(_):
                trace = fixture.traces()
                trace["summary"]["routed_keys_crc"] ^= 1
                trace["summary"]["faults"] = 1
                trace["records"] = [{"stage": 12, "a": 15, "b": 6, "c": 40, "micros": 100}]
                return trace
            with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=recovered_trace), \
                 patch("kms_stress.runner.time.monotonic", new=Clock()):
                result = runner.segment(1, 6, 10, 1, "everyday")
            runner.evidence.finish({"status": result["status"], "segments": [result]})
            self.assertNotEqual(result["first_divergence"], "Master HID decoding/routing")
            self.assertIn("recovery", result["first_divergence"])

    def test_recovered_only_trace_does_not_excuse_unexplained_overload_loss(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory))
            fixture = Fixture(runner)
            emit = fixture.emit
            fixture.emit = lambda source, sequence: None if (source, sequence) == (1, 2) else emit(source, sequence)
            runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
            runner.master_verified = True
            runner.evidence = Evidence(directory, runner.config)
            def trace(_):
                result = fixture.traces()
                result["summary"]["faults"] = 1
                result["records"] = [{"stage": 12, "a": 15, "b": 6, "c": 40, "micros": 100}]
                return result
            with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=trace), \
                 patch("kms_stress.runner.time.monotonic", new=Clock()):
                result = runner.segment(1, 6, 100, 1, "stress", overload=True)
            runner.evidence.finish({"status": result["status"], "segments": [result]})
            self.assertEqual(result["status"], "FAIL")

    def test_soak_preview_exposes_selected_rate_and_combined_load(self):
        with tempfile.TemporaryDirectory() as directory:
            c = config(directory)
            c.preset = "soak"
            controller = Controller(Path(directory) / "profile.json", c)
            workload = controller.snapshot()["workload"]
            self.assertEqual(workload["rates_per_generator"], [100])
            self.assertEqual(workload["combined_rates"], [200])
            self.assertEqual(workload["receiver_slots"], [6])
            self.assertEqual(workload["soak_seconds"], 28800)
            self.assertIn("single rate", workload["rate_selection"])

    def test_report_shows_primary_fault_and_actual_duration_without_expanding_details(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            evidence.finish({"status": "FAIL", "workload": {"combined_rates": [60]}, "segments": [
                {"slot": 6, "rate": 30, "profile": "stress", "status": "FAIL", "seconds": 60,
                 "elapsed_seconds": 28.0, "errors": ["Generator 1 release incomplete"],
                 "primary_failure": "USB control deadline", "actual_reports_per_second": {1: 30}}]})
            visible = (evidence.path / "report.html").read_text().split("<details>")[0]
            self.assertIn("USB control deadline", visible)
            self.assertIn("28.0", visible)
            self.assertIn("60", visible)

    def test_baseline_detects_stall_and_retains_partial_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            c = config(directory)
            c.preset = "baseline"
            runner = Runner(c)
            class Stuck:
                error = None
                def request(self, command, prefix=None, timeout=3):
                    if command == "bench stats": return ["KMSGEN STATS 42 1 840 840 839 0 00000000 1 1"]
                    return [prefix] if prefix else []
                def close(self): pass
            with patch("kms_stress.runner.SerialChannel", return_value=Stuck()), \
                 patch("kms_stress.runner.read_identity") as identity, patch("kms_stress.runner.Capture") as capture, \
                 patch("kms_stress.runner.secrets.randbelow", return_value=41), \
                 patch("kms_stress.runner.time.monotonic", new=Clock()), patch("kms_stress.runner.time.sleep"), \
                 patch.object(runner.cancel, "wait"), patch.object(runner, "stop_generators", return_value={}), \
                 patch.object(runner, "wait"):
                c.generators.pop()
                identity.return_value.role = "generator"
                identity.return_value.identity = c.generators[0].identity
                capture.return_value.error = None
                result, _ = runner.run()
            self.assertEqual(result["status"], "FAIL", result["message"])
            self.assertTrue(result["segments"][0]["partial"])
            self.assertIn("progress", result["message"])


if __name__ == "__main__": unittest.main()
