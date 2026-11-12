"""Progress checks cover both sources and recovery evidence boundaries."""
import unittest

from kms_stress.diagnostics import Progress, WorkloadStalled, checkpoint_boundary


def stats(completed, running=1):
    return dict(completed=completed, submitted=completed + 1, running=running, missed=7, pending=1)


class DiagnosticsTests(unittest.TestCase):
    def test_slow_progress_does_not_fail_merely_for_missing_requested_rate(self):
        monitor = Progress({1: 0})
        for count in range(1, 20): monitor.observe(1, stats(count), count * 2.5)

    def test_source_one_cannot_hide_stalled_source_two(self):
        monitor = Progress({1: 0, 2: 0})
        for count in (1, 2):
            monitor.observe(1, stats(count), count)
            monitor.observe(2, stats(0), count)
        monitor.observe(1, stats(3), 3)
        with self.assertRaisesRegex(WorkloadStalled, "Generator 2.*no USB completion progress"):
            monitor.observe(2, stats(0), 3)

    def test_new_segment_gets_own_progress_deadline(self):
        monitor = Progress({1: 20})
        monitor.observe(1, stats(0), 22.9)
        monitor.observe(1, stats(1), 23.1)

    def test_unexpected_generator_stop_is_failure(self):
        with self.assertRaisesRegex(WorkloadStalled, "stopped before"):
            Progress({1: 0}).observe(1, stats(2, running=0), 1)

    def test_capture_mismatch_remains_localized_even_after_device_fault(self):
        boundary = checkpoint_boundary([(60, 1), (60, 1), (60, 1), (59, 2)], {}, ["timeout"])
        self.assertEqual(boundary, "Linux HID capture path")

    def test_no_fault_crc_boundary_does_not_prove_decoder_bug(self):
        boundary = checkpoint_boundary([(324, 535776793), (324, 3754918337), (60, 2196959830), (60, 2196959830)], {}, [])
        self.assertIn("cause unconfirmed", boundary)


if __name__ == "__main__": unittest.main()
