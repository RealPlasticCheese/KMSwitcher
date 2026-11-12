"""Campaign planning, observed incident classification and isolated device runs."""
from copy import deepcopy
from contextlib import contextmanager
from dataclasses import asdict, replace
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_stress import campaign, usb_diagnostics as usb
from kms_stress.campaign import Case, USBCollector
from kms_stress.diagnostics import GeneratorStopError, WorkloadStalled
from kms_stress.model import Config, Device
from kms_stress.runner import Cancelled, Evidence, Runner
from kms_stress.web import parse_config
from test_runner_integration import Clock, Collector, Fixture, Master


def config(directory="unused"):
    return Config(master=Device("112233445566", uart="master"),
                  generators=[Device("AABBCCDDEE01", uart="a"), Device("AABBCCDDEE02", uart="b")],
                  receivers=[Device("AABBCCDDEE06", slot=6)], output=directory,
                  preset="usb-stall", rates=[10], seconds=1)


def status_lines(token=42, at=1000, boot="37ef0fa9", fault=1):
    return [f"KMSUSBSTATUS BEGIN id={token} boot={boot} at={at} slot=6 fault={fault}",
            "KMSUSBSTATUS STATE state=0 attempts=0 recovered=0 restarts=0 controls=3 hubs=1 inputs=2 capacity=8 sources=2 suspended=0 pending=0",
            "KMSUSBSTREAM COUNTS completed=100 pending=0 peak=2/8 dropped=0 stale=0 expired=0 overflows=0 last-complete=900",
            "KMSUSBSTREAM TIMING callback-us=100 callback-at=200 dispatch-us=200 dispatch-at=300 poll-gap-us=300 gap-at=400 decode-us=40 decode-at=500",
            f"KMSUSBSTATUS END id={token}"]


def sample(**kwargs): return usb.snapshot(status_lines(**kwargs), 42)


def observed_fault():
    path = Path(__file__).parent / "fixtures/usb-host-stall.json"
    return usb.fault_frames(json.loads(path.read_text())["lines"])[0]


def recipe(case, cfg):
    return json.dumps({"version": 1, "fixture": campaign.fixture(cfg), "case_id": case.identity, "case": asdict(case)})


class PlanTests(unittest.TestCase):
    def test_default_rates_cover_two_sources_and_all_seven_receivers(self):
        cfg = config()
        cfg.rates = Config().rates
        cfg.receivers = [Device(f"CCCCCCCC{slot:04X}", slot=slot) for slot in range(1, 8)]
        cfg.validate()
        self.assertEqual(len(campaign.cases(cfg)), 1176)
        cfg.preset = "all-tests"
        cfg.validate()
        cfg.rates.append(2)
        with self.assertRaisesRegex(ValueError, "USB campaign exceeds"):
            cfg.validate()

    def test_matrix_covers_each_source_both_sources_rates_receivers_and_activities(self):
        cfg = config(); cfg.rates = [50, 10, 50]
        cfg.receivers.append(Device("AABBCCDDEE03", slot=3))
        plan = campaign.cases(cfg)
        self.assertEqual(len(plan), 2 * 2 * 3 * 8)
        self.assertEqual({tuple(c.source_ids) for c in plan}, {(1,), (2,), (1, 2)})
        self.assertEqual({c.rate for c in plan}, {10, 50})
        self.assertEqual({c.slot for c in plan}, {3, 6})
        self.assertEqual({c.profile for c in plan}, {"stress", "pointer", "media"})
        self.assertEqual({c.activity for c in plan}, {"input", "idle-wake", "led", "background", "console", "receiver-readiness"})
        self.assertEqual(len({c.identity for c in plan}), len(plan))
        self.assertEqual(plan, campaign.cases(cfg))
        self.assertTrue(all(c.idle_seconds == cfg.campaign_idle_seconds for c in plan if c.activity == "idle-wake"))

    def test_disabled_generators_receivers_are_excluded(self):
        cfg = config(); cfg.generators[1].enabled = False
        cfg.receivers.append(Device("AABBCCDDEE03", slot=3, enabled=False))
        self.assertEqual(len(campaign.cases(cfg)), 8)
        self.assertTrue(all(tuple(c.source_ids) == (1,) and c.slot == 6 for c in campaign.cases(cfg)))

    def test_replay_uses_saved_workload_seed_duration_and_source_selection(self):
        cfg = config()
        case = Case(6, 77, 31, 0xffffffff, 1, "pointer", "idle-wake", 300, 2)
        cfg.campaign_recipe = recipe(case, cfg)
        cfg.seed, cfg.seconds, cfg.rates = 2, 10, [1]
        self.assertEqual(campaign.cases(cfg), [case])
        cfg.validate()
        preview = cfg.workload()["campaign"]
        self.assertTrue(preview["replay"])
        self.assertEqual(preview["planned_idle_seconds"], 300)

    def test_replay_rejects_tampering_fixture_reorder_disabled_source_and_wrong_slot(self):
        for mutation in ("id", "master", "order", "disabled", "slot", "version"):
            with self.subTest(mutation=mutation):
                cfg = config(); case = Case(6, 10, 1, 1, 1, "stress", "input", first_source=2)
                saved = json.loads(recipe(case, cfg))
                if mutation == "id": saved["case_id"] = "0" * 16
                elif mutation == "master": cfg.master.identity = "000000000001"
                elif mutation == "order": cfg.generators.reverse()
                elif mutation == "disabled": cfg.generators[1].enabled = False
                elif mutation == "slot":
                    case = replace(case, slot=1); saved["case"] = asdict(case); saved["case_id"] = case.identity
                else: saved["version"] = 2
                cfg.campaign_recipe = json.dumps(saved)
                with self.assertRaises(ValueError): campaign.cases(cfg)

    def test_malformed_recipe_and_bounds_are_rejected_before_hardware(self):
        cfg = config()
        for raw in ("null", "[]", "true", "{}", '{"case":null}', "not json"):
            cfg.campaign_recipe = raw
            with self.subTest(raw=raw), self.assertRaises(ValueError): cfg.validate_campaign()
        cfg.campaign_recipe = ""
        for name, value in (("campaign_repeats", 0), ("campaign_repeats", 11), ("campaign_repeats", True),
                            ("campaign_idle_seconds", -1), ("campaign_idle_seconds", 3601), ("fixture_notes", "x" * 1025)):
            with self.subTest(name=name, value=str(value)[:20]):
                changed = replace(cfg, **{name: value})
                with self.assertRaises(ValueError): changed.validate_campaign()
        cfg.receivers = [Device(f"AABBCCDDEE{slot:02X}", slot=slot) for slot in range(1, 8)]
        cfg.rates = list(range(1, 10))
        with self.assertRaisesRegex(ValueError, str(campaign.MAX_CASES)): campaign.cases(cfg)

    def test_http_profile_roundtrip_and_old_profile_defaults(self):
        cfg = config(); cfg.fixture_notes = "EDO 4 port USB hub; adjacent generators"
        cfg.campaign_recipe = recipe(campaign.cases(cfg)[0], cfg)
        self.assertEqual(parse_config(asdict(cfg)), cfg)
        old = asdict(cfg)
        for name in ("campaign_repeats", "campaign_idle_seconds", "campaign_recipe", "fixture_notes"): old.pop(name)
        restored = parse_config(old)
        self.assertEqual(restored.campaign_repeats, 3)
        self.assertEqual(restored.campaign_recipe, "")


class SnapshotTests(unittest.TestCase):
    def test_matching_frame_ignores_complete_delayed_reply(self):
        lines = status_lines(41) + ["unrelated radio message"] + status_lines(42)
        parsed = usb.snapshot(lines, 42)
        self.assertEqual(parsed["boot"], 0x37ef0fa9)
        self.assertEqual(parsed["counts"]["peak"], [2, 8])

    def test_truncated_mismatched_duplicate_or_interleaved_status_cannot_pass(self):
        lines = status_lines()
        for bad in (lines[:-1], status_lines(41), lines + lines, lines[:2] + [lines[1]] + lines[2:],
                    lines[:3] + [lines[3], lines[3]] + lines[4:],
                    lines[:2] + status_lines(41) + lines[2:], lines * 103):
            with self.subTest(lines=bad[:2]), self.assertRaises(ValueError): usb.snapshot(bad, 42)

    def test_bad_capacity_and_numeric_fields_rejected(self):
        for find, change in (("inputs=2", "inputs=8"), ("slot=6", "slot=8"), ("completed=100", "completed=-1"),
                             ("completed=100", "completed=4294967296"), ("state=0", "state=0 state=0"),
                             ("peak=2/8", "peak=2/4294967296"), ("decode-at=500", "missing=500")):
            lines = [line.replace(find, change) for line in status_lines()]
            with self.subTest(change=change), self.assertRaises(ValueError): usb.snapshot(lines, 42)

    def test_collector_rejects_changed_boot_and_stale_clock_but_accepts_wrap(self):
        runner = Mock(expected_master_boot="37ef0fa9", expected_usb_boot=None, campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
        collector = USBCollector(runner)
        def reply(command, prefix, timeout, **kwargs): return status_lines(int(command.split()[1]), at=reply.at, boot=reply.boot)
        reply.at, reply.boot = 0xfffffff0, "37ef0fa9"
        runner.master.request.side_effect = reply
        collector.snapshot(); reply.at = 10; collector.snapshot()
        reply.at = 9
        with self.assertRaisesRegex(ValueError, "Stale"): collector.snapshot()
        reply.boot = "00000001"
        with self.assertRaisesRegex(RuntimeError, "boot changed"): collector.snapshot()

    def test_poll_is_periodic_and_new_fault_stops_workload(self):
        runner = Mock(expected_master_boot="37ef0fa9", expected_usb_boot=None, campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
        collector = USBCollector(runner); collector.before = sample()
        with patch.object(collector, "snapshot", return_value=sample(fault=2)) as capture:
            collector.next_poll = float("inf")
            self.assertFalse(collector.poll()); capture.assert_not_called()
            collector.next_poll = 0
            with self.assertRaises(WorkloadStalled): collector.poll()

    def test_diagnostic_and_trace_boot_ids_are_independent_across_collectors(self):
        # Actual COM3 read-back: trace boot=139dbf5c, USB diagnostics=115a98e9.
        runner = Mock(expected_master_boot="139dbf5c", expected_usb_boot=None,
                      campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
        def reply(command, prefix, timeout, **kwargs):
            return status_lines(int(command.split()[1]), boot=reply.boot)
        reply.boot = "115a98e9"; runner.master.request.side_effect = reply
        USBCollector(runner).snapshot()
        self.assertEqual(runner.expected_usb_boot, 0x115a98e9)
        USBCollector(runner).snapshot()  # A new case retains the run's diagnostic boot baseline.
        reply.boot = "00000001"
        with self.assertRaisesRegex(RuntimeError, "boot changed"): USBCollector(runner).snapshot()

    def test_collector_checks_trace_reset_guard_before_and_after_status_read(self):
        runner = Mock(expected_master_boot="139dbf5c", expected_usb_boot=None,
                      campaign_case=Case(6, 10, 1, 1, 1, "stress", "input"))
        runner.master.request.side_effect = lambda command, prefix, timeout, **kwargs: status_lines(int(command.split()[1]), boot="115a98e9")
        runner.check.side_effect = [None, RuntimeError("Trace master restarted")]
        with self.assertRaisesRegex(RuntimeError, "Trace master restarted"): USBCollector(runner).snapshot()
        self.assertEqual(runner.check.call_count, 2)
        self.assertIsNone(runner.expected_usb_boot)

    def test_fault_parser_ignores_partial_retains_signed_control_and_rejects_invalid_header(self):
        fault = observed_fault()
        self.assertEqual(fault["sequence"], 2)
        self.assertEqual(len(fault["samples"]), 6)
        self.assertIn("status=-1", fault["pending"][0])
        self.assertEqual(usb.fault_frames(fault["lines"][:-1]), [])
        self.assertEqual(usb.fault_frames(fault["lines"][1:]), [])
        with self.assertRaises(ValueError): usb.fault_frames(["KMSUSBFAULT BEGIN 1 boot=00000001", "KMSUSBFAULT END"])

    def test_cached_fault_survives_later_manual_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(config(directory)); runner.evidence = Evidence(directory, runner.config)
            runner.master = Mock()
            fault = observed_fault()
            for line in fault["lines"]: runner.evidence.log("uart", {"port": "master", "line": line})
            runner.master.request.side_effect = TimeoutError("USB fault read timed out")
            frames, errors = USBCollector(runner).frozen()
            runner.evidence.finish({"status": "INCONCLUSIVE"})
            self.assertEqual(frames[0]["sequence"], 2)
            self.assertEqual(len(errors), 1)


class ClassificationTests(unittest.TestCase):
    def setUp(self):
        self.before, self.after = sample(at=4475000), sample(at=4478000, fault=2)
        self.fault = observed_fault()
        self.item = {"status": "FAIL", "errors": ["Generator stopped completing"], "partial": True}

    def classify(self, fault=None, after=None):
        return usb.classify(self.before, after or self.after, [self.fault if fault is None else fault], self.item)

    def test_actual_live_frames_rx_fifo_unprocessed_qtd_signature(self):
        result = self.classify()
        self.assertEqual(result["signature"], "host-no-completion-rx-fifo")
        self.assertEqual(result["cause"], "unconfirmed")
        self.assertEqual(result["fault_sequence"], 2)

    def test_required_signature_evidence_is_not_inferred(self):
        for change in ("irq-progress", "channel-irq", "stopped-clock", "no-global-irq", "no-dma", "no-qtd", "no-control", "stopped-frames", "power-loss", "missing-sample"):
            with self.subTest(change=change):
                fault = deepcopy(self.fault)
                if change == "irq-progress": fault["samples"][-1]["irqs"] += 1
                elif change == "channel-irq": fault["samples"][-1]["channels"] = 4
                elif change == "stopped-clock": fault["samples"][-1]["clock"] = 1
                elif change == "no-global-irq": fault["samples"][-1]["ahb"] &= ~1
                elif change == "no-dma": fault["samples"][-1]["ahb"] &= ~0x20
                elif change == "no-qtd": fault["qtd"] = [0, 0, 0]
                elif change == "no-control": fault["pending"] = []
                elif change == "stopped-frames": fault["samples"][-1]["frame"] = fault["samples"][-2]["frame"]
                elif change == "power-loss": fault["bus"]["hprt"] = 0
                else: fault["samples"] = fault["samples"][-1:]
                self.assertEqual(self.classify(fault)["signature"], "host-control-timeout")

    def test_old_other_boot_and_pre_attempt_faults_do_not_reproduce(self):
        for change in ("old-sequence", "other-boot", "old-time"):
            fault = deepcopy(self.fault)
            if change == "old-sequence": fault["sequence"] = 1
            elif change == "other-boot": fault["boot"] += 1
            else: fault["at"] = self.before["at"] - 1
            self.assertEqual(self.classify(fault)["signature"], "host-fault-without-frozen-evidence")

    def test_manual_fault_is_not_spontaneous_reproduction(self):
        fault = deepcopy(self.fault); fault["op"] = 255
        self.item = {"status": "PASS", "errors": []}
        self.assertEqual(self.classify(fault)["signature"], "no-failure")

    def test_clock_sequence_frame_and_counter_wrap(self):
        self.before.update(at=0xfffffff0, fault=0xffffffff)
        self.after.update(at=100, fault=1)
        fault = deepcopy(self.fault); fault.update(at=50, sequence=1)
        fault["samples"][-2]["frame"] = 0xffff; fault["samples"][-1]["frame"] = 1
        self.assertEqual(self.classify(fault)["signature"], "host-no-completion-rx-fifo")
        self.after["fault"] = self.before["fault"]
        self.before["counts"]["dropped"] = 0xffffffff; self.after["counts"]["dropped"] = 1
        self.assertEqual(usb.classify(self.before, self.after, [], self.item)["signature"], "input-queue-pressure")

    def test_windows_reboot_receiver_output_events_are_not_s3_host_stalls(self):
        self.after["fault"] = self.before["fault"]
        for event in ("USB unavailable", "peer restarted", "peer lost", "acknowledgement timeout"):
            item = {"errors": ["delivery failed"], "observed_faults": [{"event": event}]}
            result = usb.classify(self.before, self.after, [], item)
            self.assertEqual(result["signature"], "receiver-link-interruption")
            self.assertIn("unconfirmed", result["cause"])
        # A re-enumeration/temporary quiet period alone supplies no host-stall proof.
        self.assertEqual(usb.classify(self.before, self.after, [], {"errors": []})["signature"], "no-failure")

    def test_resets_capture_loss_and_recovery_without_frame_are_incomplete(self):
        self.after["boot"] += 1
        self.assertEqual(self.classify()["signature"], "master-reset")
        self.after = deepcopy(self.before)
        self.after["state"]["recovered"] += 1
        result = usb.classify(self.before, self.after, [], self.item)
        self.assertEqual(result["confidence"], "incomplete")
        self.item["collector_failure"] = True
        self.assertEqual(self.classify()["signature"], "capture-disconnected")

    def test_disappearing_inputs_and_selection_failures_are_not_dma_signature(self):
        self.after = deepcopy(self.before); self.after["state"]["inputs"] = 0
        result = usb.classify(self.before, self.after, [], {"status": "PASS", "errors": []})
        self.assertEqual(result["signature"], "host-input-topology-changed")
        self.after = deepcopy(self.before)
        item = {"status": "FAIL", "partial": True, "errors": ["Unavailable destination was accepted"]}
        self.assertEqual(usb.classify(self.before, self.after, [], item)["signature"], "unclassified-failure")


class CampaignExecutionTests(unittest.TestCase):
    @contextmanager
    def directory(self):
        self.files = []
        with tempfile.TemporaryDirectory() as directory:
            try: yield directory
            finally:
                for stream in self.files: stream.close()

    def fixture(self, directory, outcomes):
        runner = Runner(config(directory)); runner.evidence = Evidence(directory, runner.config)
        runner.master, runner.capture = Mock(), Mock()
        runner.generators = [Mock(), Mock()]
        runner.check = Mock()
        case = campaign.cases(runner.config)[0]
        runner.campaign_case = case
        runner.config.campaign_recipe = recipe(case, runner.config)
        def segment(*args, **kwargs):
            outcome = outcomes.pop(0)
            if isinstance(outcome, Exception): raise outcome
            return dict(slot=6, rate=10, seconds=1, profile="stress", status=outcome, errors=[] if outcome == "PASS" else ["stream mismatch"])
        runner.segment = Mock(side_effect=segment)
        runner.wait = Mock(); runner.collect_traces = Mock(return_value={})
        self.files.append(runner.evidence.file)
        return runner

    def execute(self, runner, **kwargs):
        with patch.object(USBCollector, "snapshot", side_effect=lambda: sample()), \
             patch.object(USBCollector, "frozen", return_value=([], [])), \
             patch("kms_stress.campaign.prepare_repeat", **kwargs) as recovery:
            campaign.run(runner)
        return recovery

    def test_healthy_case_runs_once_with_recipe_and_environment(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"])
            recovery = self.execute(runner)
            self.assertEqual(runner.segment.call_count, 1); recovery.assert_not_called()
            summary = runner.campaign_summary[0]
            self.assertFalse(summary["same_signature_seen_again"])
            self.assertEqual(summary["not_run"], 0)
            runner.evidence.finish({"status": "PASS", "segments": runner.completed_segments, "campaign": runner.campaign_summary})
            self.assertTrue((runner.evidence.path / "environment.json").exists())
            self.assertEqual(len(list(runner.evidence.path.glob("recipe-*.json"))), 1)
            self.assertIn("USB campaign", (runner.evidence.path / "report.html").read_text())

    def test_failure_repeats_same_recipe_and_reports_signature_count(self):
        with self.directory() as directory:
            runner = self.fixture(directory, [WorkloadStalled("no USB completion progress"), WorkloadStalled("no USB completion progress"), "PASS"])
            recovery = self.execute(runner)
            self.assertEqual(recovery.call_count, 2)
            summary = runner.campaign_summary[0]
            self.assertTrue(summary["same_signature_seen_again"])
            self.assertEqual(summary["failure_signatures"], {"source-completion-stall": 2})
            self.assertEqual({c.kwargs["campaign_case"].identity for c in runner.segment.call_args_list}, {summary["case_id"]})
            self.assertEqual(len(list(runner.evidence.path.glob("usb-*.json"))), 3)

    def test_missing_evidence_stops_repeats_and_marks_inconclusive(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"])
            with patch.object(USBCollector, "snapshot", return_value=sample()), \
                 patch.object(USBCollector, "frozen", return_value=([], ["UART fault read timed out"])), \
                 patch("kms_stress.campaign.prepare_repeat") as recovery:
                campaign.run(runner)
            self.assertEqual(runner.completed_segments[0]["status"], "INCONCLUSIVE")
            self.assertEqual(runner.campaign_summary[0]["not_run"], 2); recovery.assert_not_called()

    def test_failed_repeat_recovery_is_explicit_and_does_not_reset_boards(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["FAIL"])
            self.execute(runner, side_effect=RuntimeError("USB did not recover"))
            self.assertEqual(runner.campaign_summary[0]["not_run"], 2)
            self.assertEqual(runner.campaign_summary[0]["repeat_blocked"], "USB did not recover")

    def test_unclassified_failures_do_not_claim_same_stall_reproduced(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["FAIL", "FAIL", "FAIL"])
            self.execute(runner)
            self.assertFalse(runner.campaign_summary[0]["same_signature_seen_again"])

    def test_observed_frozen_fault_overrides_stream_pass(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"]); runner.config.campaign_repeats = 1
            before, after = sample(at=4475000), sample(at=4478000, fault=2)
            with patch.object(USBCollector, "snapshot", side_effect=[before, after]), \
                 patch.object(USBCollector, "frozen", return_value=([observed_fault()], [])):
                campaign.run(runner)
            item = runner.completed_segments[0]
            self.assertEqual(item["status"], "FAIL")
            self.assertEqual(item["usb_classification"]["signature"], "host-no-completion-rx-fifo")

    def test_missing_after_snapshot_cannot_pass(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"])
            with patch.object(USBCollector, "snapshot", side_effect=[sample(), TimeoutError("UART disappeared")]), \
                 patch.object(USBCollector, "frozen", return_value=([], [])):
                campaign.run(runner)
            self.assertEqual(runner.completed_segments[0]["status"], "INCONCLUSIVE")
            self.assertEqual(runner.campaign_summary[0]["not_run"], 2)

    def test_cancel_during_recovery_propagates(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["FAIL"])
            with self.assertRaises(Cancelled): self.execute(runner, side_effect=Cancelled("operator stop"))

    def test_normal_inflight_control_probe_does_not_force_recovery(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"])
            pending = sample(); pending["state"]["pending"] = 1
            with patch.object(USBCollector, "snapshot", return_value=pending), \
                 patch.object(USBCollector, "frozen", return_value=([], [])):
                campaign.run(runner)
            self.assertEqual(runner.completed_segments[0]["status"], "PASS")
            collector = Mock(before=pending); collector.snapshot.return_value = pending
            self.assertTrue(campaign.healthy(runner, collector))

    def test_old_firmware_cannot_run_unframed_campaign(self):
        with self.directory() as directory:
            runner = self.fixture(directory, ["PASS"])
            runner.master.request.return_value = ["legacy human status"]
            with self.assertRaises(ValueError): campaign.run(runner)
            runner.segment.assert_not_called()
            self.assertEqual(runner.campaign_summary[0]["not_run"], 3)

    def test_stall_freezes_before_stopping_generators(self):
        with self.directory() as directory:
            runner = self.fixture(directory, [WorkloadStalled("no USB completion progress")])
            order = Mock(); order.attach_mock(runner.master, "master"); order.attach_mock(runner.capture, "capture")
            for i, gen in enumerate(runner.generators): order.attach_mock(gen, f"source{i}")
            runner.config.campaign_repeats = 1
            self.execute(runner)
            calls = [str(c) for c in order.mock_calls]
            frozen = next(i for i, c in enumerate(calls) if "bench freeze" in c)
            stopped = next(i for i, c in enumerate(calls) if "bench stop" in c)
            self.assertLess(frozen, stopped)
            self.assertEqual(runner.completed_segments[0]["usb_classification"]["signature"], "source-completion-stall")

    def test_repeat_checks_inventory_and_clears_test_overrides_without_reset(self):
        with self.directory() as directory:
            runner = self.fixture(directory, [])
            collector = Mock(before=sample())
            collector.snapshot.side_effect = [dict(sample(), state={**sample()["state"], "state": 1}), sample()]
            with patch("kms_stress.runner.verify_inventory") as inventory, patch.object(runner, "stop_generators") as stopped:
                campaign.prepare_repeat(runner, collector)
            runner.master.request.assert_any_call("usb-recover", "KMSUSB RESULT", timeout=3)
            runner.capture.feature.assert_any_call(6, 6, 0)
            runner.capture.leds.assert_called_with(6, 0)
            stopped.assert_called_once(); inventory.assert_called_once()
            self.assertFalse(any("reset" in str(c) for c in runner.master.mock_calls))


class SourceSelectionIntegrationTests(unittest.TestCase):
    def test_real_runner_and_oracle_accept_each_single_source_and_pair(self):
        for first, count in ((1, 1), (2, 1), (1, 2)):
            with self.subTest(first=first, count=count), tempfile.TemporaryDirectory() as directory:
                cfg = config(directory); runner = Runner(cfg); fixture = Fixture(runner)
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.master_verified = True; runner.evidence = Evidence(directory, cfg)
                case = Case(6, 10, 1, 0x12345678, count, "stress", "input", first_source=first)
                with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                     patch("kms_stress.runner.time.monotonic", new=Clock()):
                    result = runner.segment(1, 6, 10, 1, "stress", campaign_case=case)
                runner.evidence.finish({"status": result["status"], "segments": [result]})
                self.assertEqual(result["errors"], [])
                self.assertEqual(set(result["generators"]), set(case.source_ids))
                self.assertEqual(set(result["inactive_generators"]), {1, 2} - set(case.source_ids))

    def test_led_feedback_failure_and_inactive_source_traffic_are_detected(self):
        for failure in ("led", "inactive"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                cfg = config(directory); runner = Runner(cfg); fixture = Fixture(runner)
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.capture.leds = Mock(); runner.master_verified = True; runner.evidence = Evidence(directory, cfg)
                case = Case(6, 10, 1, 1, 1, "stress", "led" if failure == "led" else "input")
                for channel in fixture.sources:
                    original = channel.request
                    def request(command, prefix=None, timeout=3, original=original):
                        if command == "bench leds": return ["KMSGEN LEDS 0"]
                        return original(command, prefix, timeout)
                    channel.request = request
                stop = runner.stop_generators
                def stats(run):
                    value = stop(run)
                    if failure == "inactive": value[2]["offered"] = 1
                    return value
                with patch.object(runner, "wait"), patch.object(runner, "stop_generators", side_effect=stats), \
                     patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                     patch("kms_stress.runner.time.monotonic", new=Clock()):
                    result = runner.segment(1, 6, 10, 1, "stress", campaign_case=case)
                runner.evidence.finish({"status": result["status"], "segments": [result]})
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(any(("LED feedback" if failure == "led" else "Inactive generator") in e for e in result["errors"]))
                if failure == "led": runner.capture.leds.assert_called_with(6, 0)


if __name__ == "__main__": unittest.main()
