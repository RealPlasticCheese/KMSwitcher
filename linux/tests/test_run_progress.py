"""Countdown coverage for every preset, without opening the hardware fixture."""
import math
import tempfile
import unittest
from unittest.mock import patch

from kms_stress.model import Config, Device, PRESETS
from kms_stress.runner import Cancelled, Runner
from kms_stress.run_progress import RunProgress


def config(preset):
    result = Config(master=Device("112233445566", uart="master"),
                  generators=[Device("AABBCCDDEE01", uart="generator")],
                  receivers=[Device("AABBCCDDEE02", slot=1), Device("AABBCCDDEE03", slot=2)],
                  preset=preset, rates=[10, 100], seconds=10, soak_seconds=60)
    if preset == "mixed-soak":
        result.generators.append(Device("AABBCCDDEE04", uart="second-generator"))
    return result


class CountdownTests(unittest.TestCase):
    def test_every_preset_exposes_finite_progress_during_preparation_and_cleanup(self):
        for preset in PRESETS:
            with self.subTest(preset=preset), tempfile.TemporaryDirectory() as directory:
                c = config(preset)
                c.output = directory
                runner = Runner(c)
                def preparing(*_):
                    value = runner.progress()
                    self.assertEqual(value["phase"], "preparing")
                    self.assertGreater(value["remaining_seconds"], 0)
                    self.assertTrue(math.isfinite(value["remaining_seconds"]))
                    self.assertEqual(value["estimated"], preset not in ("soak", "all-tests"))
                    raise Cancelled("Stopped during preparation")
                with patch.object(runner, "preflight", side_effect=preparing), \
                     patch.object(runner, "baseline", side_effect=preparing):
                    result, _ = runner.run()
                self.assertEqual(result["status"], "CANCELLED")
                value = runner.progress()
                self.assertEqual(value["phase"], "finished")
                self.assertFalse(value["running"])
                self.assertEqual(value["status"], "CANCELLED")
                self.assertEqual(value["remaining_seconds"], 0)

    def test_finite_presets_count_down_across_segments_and_live_handovers(self):
        presets = set(PRESETS) - {"baseline", "soak", "all-tests", "usb-stall"}
        for preset in presets:
            with self.subTest(preset=preset), tempfile.TemporaryDirectory() as directory:
                c = config(preset)
                c.output = directory
                runner = Runner(c)
                now = 100.0
                remaining = []
                def segment(index, slot, rate, seconds, profile, *args):
                    nonlocal now
                    value = runner.progress()
                    self.assertEqual(value["phase"], "running")
                    self.assertEqual(value["slot"], slot)
                    self.assertGreater(value["remaining_seconds"], 0)
                    remaining.append(value["remaining_seconds"])
                    now += seconds / 2
                    self.assertEqual(runner.progress()["remaining_seconds"], remaining[-1] - seconds / 2)
                    now += seconds / 2 + 30  # Slow trace downloads must not consume the next segment.
                    return dict(slot=slot, rate=rate, seconds=seconds, profile=profile, status="PASS", errors=[])
                def handover(_):
                    self.assertEqual(runner.progress()["test"], "live handover")
                    self.assertEqual(runner.progress()["remaining_seconds"], 6)
                    return dict(slot="all", rate=10, profile="continuous handover safety", status="PASS", errors=[])
                with patch("kms_stress.run_progress.time.monotonic", side_effect=lambda: now), \
                     patch.object(runner, "preflight"), patch.object(runner, "segment", side_effect=segment), \
                     patch.object(runner, "live_handover", side_effect=handover):
                    result, _ = runner.run()
                self.assertEqual(result["status"], "PASS")
                self.assertEqual(remaining, sorted(remaining, reverse=True))
                self.assertEqual(runner.progress()["counts"]["PASS"], len(result["segments"]))

    def test_soak_countdown_uses_the_schedule_deadline_without_limiting_segments(self):
        runner = Runner(config("soak"))
        runner.run_progress = RunProgress(60, "soak", timed=True)
        now = 100.0
        with patch("kms_stress.runner.time.monotonic", side_effect=lambda: now):
            schedule = runner.schedule()
            first = next(schedule)
            self.assertEqual(runner.progress()["remaining_seconds"], 60)
            now += 12
            self.assertEqual(runner.progress()["remaining_seconds"], 48)
            next(schedule)
            self.assertEqual(runner.progress()["remaining_seconds"], 48)
            self.assertIsNone(runner.timed_deadline)
            self.assertEqual(first[2], 10)

    def test_campaign_estimate_includes_idle_and_grows_only_for_added_retries(self):
        from kms_stress.campaign import cases
        runner = Runner(config("usb-stall"))
        plan = cases(runner.config)
        expected = sum(case.seconds + case.idle_seconds for case in plan)
        self.assertEqual(runner.planned_seconds(), expected)
        runner.run_progress = RunProgress(expected, "usb-stall")
        duration = plan[0].seconds + plan[0].idle_seconds
        with patch("kms_stress.run_progress.time.monotonic", return_value=100):
            runner.begin_progress_step(duration, "USB input", 1)
            self.assertEqual(runner.progress()["remaining_seconds"], expected)
            runner.end_progress_step()
            self.assertEqual(runner.progress()["remaining_seconds"], expected - duration)
            runner.begin_progress_step(duration, "USB retry", 1, extra=True)
            self.assertEqual(runner.progress()["remaining_seconds"], expected)
            runner.end_progress_step()
            self.assertEqual(runner.progress()["remaining_seconds"], expected - duration)

    def test_baseline_estimate_counts_each_configured_rate_once(self):
        runner = Runner(config("baseline"))
        self.assertEqual(runner.planned_seconds(), 20)

    def test_suite_keeps_live_deadline_and_exposes_cleanup_and_final_result(self):
        runner = Runner(config("all-tests"))
        runner.run_progress = RunProgress(60, "all-tests", timed=True)
        runner.run_progress.start_timed(160)
        runner.timed_deadline = 160
        runner.suite_progress = dict(running=True, cycle=2, test="pointer", slot=1, counts={})
        with patch("kms_stress.runner.time.monotonic", return_value=125):
            self.assertEqual(runner.progress()["remaining_seconds"], 35)
            self.assertEqual(runner.progress()["cycle"], 2)
            runner.run_progress.cleanup()
            self.assertEqual(runner.progress()["phase"], "cleanup")
            runner.run_progress.finish("PASS")
            self.assertEqual(runner.progress()["phase"], "finished")
            self.assertFalse(runner.progress()["running"])
            self.assertEqual(runner.progress()["remaining_seconds"], 0)


if __name__ == "__main__":
    unittest.main()
