"""Synthetic USB faults and transitions between workload cases."""
import itertools
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_stress import campaign, usb_diagnostics as usb
from kms_stress.campaign import Case
from kms_stress.diagnostics import GeneratorStopError, WorkloadStalled
from kms_stress.runner import Cancelled, Evidence, Runner
from test_runner_integration import Clock, Collector, Fixture, Master
import test_usb_campaign as campaign_tests
from test_usb_campaign import config, sample


def incident():
    return json.loads((Path(__file__).parent / "fixtures/usb-campaign.json").read_text())


class IdleCounterTests(unittest.TestCase):
    def test_active_idle_transition_matrix_preserves_old_firmware_counters(self):
        # The old adapter incorrectly reset counters on config, hiding this bug.
        choices = ((1, 1), (2, 1), (1, 2))
        for previous, current in itertools.product(choices, repeat=2):
            with self.subTest(previous=previous, current=current), tempfile.TemporaryDirectory() as directory:
                runner = Runner(config(directory)); fixture = Fixture(runner)
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.master_verified = True; runner.evidence = Evidence(directory, runner.config)
                try:
                    with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                         patch("kms_stress.runner.time.monotonic", new=Clock()):
                        for index, (first, count) in enumerate((previous, current), 1):
                            case = Case(6, 10, 1, 1, count, "stress", "input", first_source=first)
                            result = runner.segment(index, 6, 10, 1, "stress", campaign_case=case)
                            self.assertEqual(result["errors"], [])
                            for source, before in result.get("inactive_generator_baselines", {}).items():
                                self.assertEqual(before, result["inactive_generators"][source])
                finally: runner.evidence.file.close()

    def test_non_neutral_idle_baseline_blocks_starting_active_source(self):
        for flag in ("running", "pending"):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as directory:
                runner = Runner(config(directory)); fixture = Fixture(runner)
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.master_verified = True; runner.evidence = Evidence(directory, runner.config)
                original = fixture.sources[1].request
                def request(command, prefix=None, timeout=3):
                    if command == "bench stats":
                        return [f"KMSGEN STATS {fixture.run} 2 0 0 0 0 00000000 {int(flag == 'running')} {int(flag == 'pending')}"]
                    return original(command, prefix, timeout)
                fixture.sources[1].request = request
                fixture.sources[0].request = Mock(wraps=fixture.sources[0].request)
                try:
                    with patch.object(runner, "wait"), self.assertRaisesRegex(WorkloadStalled, "not neutral before input"):
                        runner.segment(1, 6, 10, 1, "stress", campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
                    self.assertFalse(any(c.args[0] == "bench start" for c in fixture.sources[0].request.call_args_list))
                finally: runner.evidence.file.close()

    def test_synthetic_idle_totals_are_unchanged_from_previous_active_case(self):
        data = incident()
        self.assertEqual(data["previous_active"], {**data["inactive_after"], "run": data["previous_active"]["run"]})
        self.assertEqual(data["inactive_after"]["completed"], 101)

    def test_idle_counter_and_crc_changes_cannot_pass(self):
        for field in ("offered", "submitted", "completed", "missed", "crc"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                runner = Runner(config(directory)); fixture = Fixture(runner)
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.master_verified = True; runner.evidence = Evidence(directory, runner.config)
                stop = runner.stop_generators
                def changed(run):
                    stats = stop(run); stats[2][field] += 1; return stats
                try:
                    with patch.object(runner, "wait"), patch.object(runner, "stop_generators", side_effect=changed), \
                         patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                         patch("kms_stress.runner.time.monotonic", new=Clock()):
                        result = runner.segment(1, 6, 10, 1, "stress", campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
                    self.assertIn("Inactive generator 2 produced traffic", result["errors"])
                finally: runner.evidence.file.close()


class RecoveryTests(unittest.TestCase):
    directory = campaign_tests.CampaignExecutionTests.directory
    fixture = campaign_tests.CampaignExecutionTests.fixture
    def test_last_failed_attempt_recovers_before_next_recipe(self):
        for outcome in (None, RuntimeError("neutral release incomplete"), Cancelled("operator stop")):
            with self.subTest(outcome=outcome), self.directory() as directory:
                runner = self.fixture(directory, [WorkloadStalled("no USB completion progress"), "PASS"])
                plan = campaign.cases(config(directory))[:2]
                runner.config.campaign_recipe = ""; runner.config.campaign_repeats = 1
                runner.config.stop_on_failure = False
                order = Mock(); order.attach_mock(runner.segment, "segment")
                with patch("kms_stress.campaign.cases", return_value=plan), \
                     patch.object(campaign.USBCollector, "snapshot", return_value=sample()), \
                     patch.object(campaign.USBCollector, "frozen", return_value=([], [])), \
                     patch("kms_stress.campaign.prepare_repeat", side_effect=outcome) as recover:
                    order.attach_mock(recover, "recover")
                    if isinstance(outcome, Cancelled):
                        with self.assertRaises(Cancelled): campaign.run(runner)
                    else: campaign.run(runner)
                self.assertEqual(runner.segment.call_count, 1 if outcome else 2)
                self.assertEqual([c[0] for c in order.mock_calls], ["segment", "recover"] + ([] if outcome else ["segment"]))
                if outcome and not isinstance(outcome, Cancelled):
                    self.assertEqual(runner.campaign_summary[0]["remaining_cases_not_run"], 1)

    def test_pending_generator_requires_recovery_even_with_healthy_host_inventory(self):
        for second_stop_fails in (False, True):
            with self.subTest(second_stop_fails=second_stop_fails), self.directory() as directory:
                runner = self.fixture(directory, [])
                stats = incident()["stalled_generator"]
                failure = GeneratorStopError({2: stats}, ["Generator 2: neutral release incomplete"])
                runner.stop_generators = Mock(side_effect=[failure, failure if second_stop_fails else {}])
                collector = Mock(before=sample()); collector.snapshot.return_value = sample()
                with patch("kms_stress.runner.verify_inventory") as inventory:
                    if second_stop_fails:
                        with self.assertRaises(GeneratorStopError): campaign.prepare_repeat(runner, collector)
                    else: campaign.prepare_repeat(runner, collector)
                self.assertEqual(runner.stop_generators.call_count, 2)
                self.assertEqual(sum(c.args[0] == "usb-recover" for c in runner.master.request.call_args_list), 1)
                self.assertEqual(inventory.call_count, 0 if second_stop_fails else 1)
                self.assertFalse(any("reset" in str(c) for c in runner.master.mock_calls))

    def test_failed_stop_after_reconnect_does_not_loop(self):
        with self.directory() as directory:
            runner = self.fixture(directory, [])
            runner.stop_generators = Mock(side_effect=GeneratorStopError({2: incident()["stalled_generator"]}, ["pending"]))
            collector = Mock(before=sample())
            collector.snapshot.side_effect = [dict(sample(), state={**sample()["state"], "state": 1}), sample()]
            with self.assertRaises(GeneratorStopError): campaign.prepare_repeat(runner, collector)
            self.assertEqual(sum(c.args[0] == "usb-recover" for c in runner.master.request.call_args_list), 1)


class SyntheticStallTests(unittest.TestCase):
    def test_actual_synthetic_generator_stall_matches_host_completion_signature_without_rx_fifo(self):
        data = incident(); fault = usb.fault_frames(data["fault_lines"])
        result = usb.classify(data["before"], data["after"], fault, {"status": "FAIL", "partial": True, "errors": []})
        self.assertEqual(result["signature"], "host-no-completion")
        self.assertEqual(result["cause"], "unconfirmed")
        self.assertTrue(all(not s["core"] & 0x10 for s in fault[0]["samples"]))

    def test_any_missing_completion_signature_evidence_falls_back_to_timeout(self):
        data = incident()
        for change in ("frames", "irqs", "clock", "dma", "irq-disabled", "channel-irq", "qtd", "power", "pending"):
            with self.subTest(change=change):
                fault = usb.fault_frames(data["fault_lines"])[0]
                a, b = fault["samples"][-2:]
                if change == "frames": b["frame"] = a["frame"]
                elif change == "irqs": b["irqs"] += 1
                elif change == "clock": b["clock"] |= 1
                elif change == "dma": b["ahb"] &= ~0x20
                elif change == "irq-disabled": b["ahb"] &= ~1
                elif change == "channel-irq": b["channels"] = 1
                elif change == "qtd": fault["qtd"] = []
                elif change == "power": fault["bus"]["hprt"] = 0
                else: fault["pending"] = []
                result = usb.classify(data["before"], data["after"], [fault], {"errors": []})
                self.assertEqual(result["signature"], "host-control-timeout")


if __name__ == "__main__": unittest.main()
