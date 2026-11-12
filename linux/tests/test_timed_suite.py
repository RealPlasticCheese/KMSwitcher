"""Deadline, breadth, failure retention and recovery tests without hardware."""
from dataclasses import asdict
import unittest
from unittest.mock import Mock, patch

from kms_stress.model import Config, Device
from kms_stress.runner import Cancelled, DurationReached, Evidence, Runner
from kms_stress.suite import Test, plan, recover, run
from kms_stress.web import parse_config


def config():
    return Config(preset="all-tests", master=Device("112233445566", uart="master"),
                  generators=[Device("AABBCCDDEE01", uart="one"), Device("AABBCCDDEE02", uart="two")],
                  receivers=[Device("AABBCCDDEE03", slot=3), Device("AABBCCDDEE06", slot=6)],
                  rates=[10, 100], seconds=3, soak_seconds=8, stop_on_failure=False)


class PlanTests(unittest.TestCase):
    def test_all_compatible_families_and_every_directed_pair(self):
        tests = plan(config())
        self.assertEqual({t.name for t in tests}, {"everyday", "pointer", "media", "overlap", "resilience",
                         "background", "routing", "live-handover", "clipboard", "clipboard-recovery", "limit", "overload", "overload-recovery", "usb-stall"})
        self.assertEqual({(t.start_slot, t.slot) for t in tests if t.name == "routing"}, {(3, 6), (6, 3)})
        self.assertEqual({(t.rate, t.slot) for t in tests if t.name == "limit"}, {(10, 3), (10, 6), (100, 3), (100, 6)})
        self.assertGreaterEqual(len({t.name for t in tests[:14]}), 13)
        self.assertEqual(len({t.identity for t in tests}), len(tests))

    def test_overload_is_immediately_followed_by_low_rate_recovery(self):
        tests = plan(config())
        for i, test in enumerate(tests):
            if test.name == "overload":
                self.assertEqual((tests[i + 1].name, tests[i + 1].slot, tests[i + 1].rate),
                                 ("overload-recovery", test.slot, 10))

    def test_usb_matrix_includes_each_source_and_idle_led_readiness(self):
        tests = [t.usb for t in plan(config()) if t.usb]
        self.assertEqual({tuple(t.source_ids) for t in tests}, {(1,), (2,), (1, 2)})
        self.assertTrue({"idle-wake", "led", "receiver-readiness", "console"} <= {t.activity for t in tests})

    def test_single_receiver_does_not_claim_switching_coverage(self):
        c = config(); c.receivers = c.receivers[:1]
        self.assertFalse(any(t.name in ("routing", "live-handover") for t in plan(c)))

    def test_old_profiles_default_recovery_and_preview_exposes_total_duration(self):
        data = asdict(config()); del data["recovery_attempts"]
        c = parse_config(data); c.validate()
        self.assertEqual(c.recovery_attempts, 3)
        self.assertEqual(c.workload()["soak_seconds"], 8)
        self.assertIn("baseline requires different wiring", c.workload_text())

    def test_invalid_recovery_limits_and_replay_are_rejected(self):
        for value in (0, 11, True, "3"):
            c = config(); c.recovery_attempts = value
            with self.assertRaises(ValueError): c.validate()
        c = config(); c.campaign_recipe = "{}"
        with self.assertRaisesRegex(ValueError, "Clear the USB replay"): c.validate()


class LoopTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.0
        self.runner = Runner(config())
        self.runner.evidence = Mock()
        self.tests = [Test("everyday", 3, 10, 3), Test("pointer", 6, 10, 3, "pointer")]
        self.calls = []

    def execute(self, runner, test, status="PASS"):
        self.calls.append((test.name, runner.config.seed))
        self.now += min(2, runner.remaining_seconds())
        runner.completed_segments.append(dict(slot=test.slot, rate=test.rate, profile=test.profile,
                                               status=status, errors=[]))

    def execute_with(self, callback, recovery=None):
        with patch("kms_stress.suite.time.monotonic", side_effect=lambda: self.now), \
             patch("kms_stress.suite.plan", return_value=self.tests), \
             patch("kms_stress.suite.execute", side_effect=callback), \
             patch("kms_stress.suite.recover", side_effect=recovery) as restored:
            run(self.runner)
        return restored

    def test_cycles_seed_variation_and_deadline_without_restarting_a_case(self):
        self.runner.config.soak_seconds = 5
        self.execute_with(self.execute)
        self.assertEqual(self.calls, [("everyday", 1), ("pointer", 1), ("everyday", 2)])
        self.assertEqual(self.runner.config.seed, 1)
        self.assertEqual(self.runner.suite_summary["completed_cycles"], 1)
        self.assertEqual(self.runner.suite_summary["elapsed_seconds"], 5)
        self.assertEqual(self.runner.suite_summary["counts"], {"PASS": 3})
        self.assertFalse(self.runner.progress()["running"])

    def test_single_generator_excludes_two_source_claims_but_keeps_pattern_checks(self):
        self.runner.config.generators[1].enabled = False
        self.assertTrue(any(t.name == "overlap" for t in plan(self.runner.config)))
        self.execute_with(self.execute)
        summary = self.runner.suite_summary
        self.assertIn("Single-source input", summary["additional_checks"])
        self.assertNotIn("Each generator alone and both together", summary["additional_checks"])
        self.assertTrue(any("shared-key/button merging" in text for text in summary["excluded"]))

    def test_two_enabled_generators_retain_two_source_matrix(self):
        self.execute_with(self.execute)
        self.assertIn("Each generator alone and both together", self.runner.suite_summary["additional_checks"])
        self.assertFalse(any("two enabled generators" in text for text in self.runner.suite_summary["excluded"]))

    def test_deadline_before_next_case_records_unvisited_coverage(self):
        self.runner.config.soak_seconds = 1
        self.execute_with(self.execute)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.runner.suite_summary["not_run"][0]["test"], "pointer")
        self.assertEqual(self.runner.suite_summary["completed_cycles"], 0)

    def test_run_verdict_requires_a_complete_attempt_for_each_planned_case(self):
        import tempfile
        for duration, partial_last, expected in ((2, False, "INCONCLUSIVE"),
                                                 (4, True, "INCONCLUSIVE"),
                                                 (4, False, "PASS")):
            with self.subTest(duration=duration, partial_last=partial_last), tempfile.TemporaryDirectory() as directory:
                self.runner = Runner(config())
                self.runner.config.output = directory
                self.runner.config.soak_seconds = duration
                self.now = 100
                def execute(runner, test):
                    self.execute(runner, test)
                    if partial_last and test == self.tests[-1]:
                        runner.completed_segments[-1]["partial"] = True
                with patch.object(self.runner, "preflight"), \
                     patch("kms_stress.suite.time.monotonic", side_effect=lambda: self.now), \
                     patch("kms_stress.suite.plan", return_value=self.tests), \
                     patch("kms_stress.suite.execute", side_effect=execute):
                    result, _ = self.runner.run()
                self.assertEqual(result["status"], expected)
                self.assertEqual(len(result["suite"]["not_run"]), int(duration == 2))
                self.assertEqual(len(result["suite"]["incomplete"]), int(partial_last))
                self.assertEqual(result["suite"]["completed_cycles"], int(expected == "PASS"))

    def test_failure_is_retained_and_recovery_precedes_the_next_case(self):
        self.runner.config.soak_seconds = 4
        order = []
        def execute(runner, test):
            order.append(test.name)
            self.execute(runner, test, "FAIL" if test.name == "everyday" else "PASS")
        def recovery(runner): order.append("recovery")
        self.execute_with(execute, recovery)
        self.assertEqual(order, ["everyday", "recovery", "pointer"])
        self.assertEqual(self.runner.suite_summary["counts"], {"FAIL": 1, "PASS": 1})

    def test_failure_at_deadline_does_not_count_a_completed_cycle(self):
        self.runner.config.soak_seconds = 2
        restored = self.execute_with(lambda runner, test: self.execute(runner, test, "FAIL"))
        restored.assert_not_called()
        self.assertEqual(self.runner.suite_summary["counts"], {"FAIL": 1})
        self.assertEqual(self.runner.suite_summary["completed_cycles"], 0)
        self.assertEqual(len(self.runner.suite_summary["not_run"]), 1)

    def test_recovery_exhaustion_is_bounded_and_never_skips_to_more_input(self):
        with self.assertRaisesRegex(RuntimeError, "reliable capture"):
            self.execute_with(lambda r, t: self.execute(r, t, "INCONCLUSIVE"),
                              Mock(side_effect=RuntimeError("capture still unavailable")))
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(len(self.runner.suite_summary["recoveries"]), 3)
        self.assertEqual(self.runner.suite_summary["stop_reason"], "recovery exhausted")

    def test_stop_on_failure_and_cancel_do_not_attempt_recovery(self):
        self.runner.config.stop_on_failure = True
        restored = self.execute_with(lambda r, t: self.execute(r, t, "FAIL"))
        restored.assert_not_called()
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.runner.suite_summary["stop_reason"], "stop on failure")
        self.runner.cancel.set()
        with self.assertRaises(Cancelled): self.execute_with(self.execute)
        self.assertEqual(self.runner.suite_summary["stop_reason"], "cancelled")

    def test_capture_loss_between_cases_is_synthetic_and_recovered(self):
        self.runner.config.soak_seconds = 4
        checks = iter([RuntimeError("Linux input event queue overrun"), None, None])
        def check():
            error = next(checks, None)
            if error: raise error
        self.runner.check = check
        def failure(runner, test, error, index):
            self.now += 2
            runner.completed_segments.append(dict(slot=test.slot, rate=test.rate, profile=test.profile,
                                                   status="INCONCLUSIVE", errors=[str(error)]))
        with patch("kms_stress.suite.save_failure", side_effect=failure):
            restored = self.execute_with(self.execute)
        self.assertEqual(restored.call_count, 1)
        self.assertEqual(self.runner.suite_summary["counts"], {"INCONCLUSIVE": 1, "PASS": 1})

    def test_deadline_during_setup_is_not_reported_as_a_failed_case(self):
        def expired(*_):
            self.now += 8
            raise DurationReached()
        restored = self.execute_with(expired)
        restored.assert_not_called()
        self.assertEqual(self.runner.completed_segments, [])
        self.assertEqual(len(self.runner.suite_summary["not_run"]), 2)

    def test_overload_without_its_required_followup_cannot_pass(self):
        self.runner.config.soak_seconds = 1
        self.tests = [Test("overload", 3, 100, 3, "stress", overload=True)]
        def execute(runner, test):
            self.execute(runner, test, "OVERLOAD OBSERVED")
            runner.completed_segments[-1]["requires_recovery_check"] = True
        self.execute_with(execute)
        item = self.runner.completed_segments[0]
        self.assertEqual(item["status"], "INCONCLUSIVE")
        self.assertEqual(item["recovery_check_status"], "not run")

    def test_report_contains_results_and_unvisited_case_count(self):
        import tempfile
        from pathlib import Path
        self.runner.config.soak_seconds = 1
        self.execute_with(self.execute)
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, self.runner.config)
            evidence.finish(dict(status="PASS", segments=self.runner.completed_segments, suite=self.runner.suite_summary))
            text = (evidence.path / "report.html").read_text(encoding="utf-8")
            self.assertIn("Timed loop coverage", text)
            self.assertIn("Unvisited cases: 1", text)


class RecoveryTests(unittest.TestCase):
    def test_identity_mismatch_prevents_any_routing_or_new_capture(self):
        runner = Runner(config()); runner.master = Mock(); runner.capture = Mock()
        wrong = Mock(role="master", identity="FFFFFFFFFFFF")
        with patch("kms_stress.suite.read_identity", return_value=wrong), patch("kms_stress.suite.Capture") as capture:
            with self.assertRaisesRegex(RuntimeError, "identity changed"): recover(runner)
        runner.master.request.assert_not_called(); capture.assert_not_called()

    def test_recovery_closes_old_queues_and_reopens_only_after_neutral_sources(self):
        runner = Runner(config()); runner.master = Mock(); runner.generators = [Mock(), Mock()]
        runner.evidence = Mock()
        runner.capture = Mock(); old = runner.capture
        runner.cancel = Mock(is_set=Mock(return_value=False))
        runner.stop_generators = Mock(); runner.wait = Mock(); runner.check = Mock()
        identities = [Mock(role="master", identity=runner.config.master.identity, boot="12345678")]
        identities += [Mock(role="generator", identity=d.identity) for d in runner.config.generators]
        frozen = set()
        def feature(slot, operation, value=0):
            if operation == 1: frozen.discard(slot)
            elif operation == 2: frozen.add(slot)
        def page(slot, index):
            if slot not in frozen: raise ValueError("Trace must be frozen before download")
        with patch("kms_stress.suite.read_identity", side_effect=identities), \
             patch("kms_stress.runner.verify_inventory"), patch("kms_stress.suite.Capture") as capture:
            capture.return_value.feature.side_effect = feature
            capture.return_value.page.side_effect = page
            recover(runner)
        runner.stop_generators.assert_called_once()
        old.close.assert_called_once(); capture.return_value.open.assert_called_once()
        self.assertIs(runner.capture, capture.return_value)
        self.assertEqual(runner.expected_master_boot, "12345678")
        self.assertIn(("test-mode on", "KMSTEST MODE test"), [c.args for c in runner.master.request.call_args_list])


if __name__ == "__main__": unittest.main()
