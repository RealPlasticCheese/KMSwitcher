"""Replay the synthetic diagnostic burst with a deterministic serial clock."""
import json
from pathlib import Path
import queue
import threading
import unittest
from unittest.mock import Mock, patch

from kms_stress.campaign import Case, USBCollector
from kms_stress.diagnostics import WorkloadStalled
from kms_stress.hardware import SerialChannel
from kms_stress.model import Config
from kms_stress.runner import Cancelled, Runner
from test_usb_campaign import sample


class Clock:
    now = 0.0
    def __call__(self): return self.now


class TimedLines:
    def __init__(self, clock, events): self.clock, self.events = clock, list(events)
    def get_nowait(self): raise queue.Empty
    def get(self, timeout):
        until = self.clock.now + timeout
        if self.events and self.events[0]["seconds"] <= until:
            item = self.events.pop(0); self.clock.now = max(self.clock.now, item["seconds"])
            return item["line"]
        self.clock.now = until; raise queue.Empty


class DelayedStatusTests(unittest.TestCase):
    def setUp(self):
        self.synthetic = json.loads((Path(__file__).parent / "fixtures/usb-delayed-status.json").read_text())
        self.clock = Clock(); self.clock.now = 0
        self.channel = SerialChannel.__new__(SerialChannel)
        self.channel.path, self.channel.error = "test master", None
        self.channel.command_lock = threading.Lock()
        self.channel.log = Mock(); self.channel.port = Mock()
        self.channel.lines = TimedLines(self.clock, self.synthetic["events"])
        self.channel.restarted = False; self.channel.last_boot_id = None
        self.runner = Runner(Config())
        self.runner.master = self.channel
        self.runner.generators = [Mock(error=None), Mock(error=None)]
        self.runner.started = {1: 0, 2: 0}; self.runner.segment_open = True
        self.runner.evidence = Mock()
        self.runner.campaign_case = Case(2, 30, 60, 1, 2, "stress", "console")
        self.pings = [[], []]
        for index, channel in enumerate(self.runner.generators):
            channel.request.side_effect = lambda *args, index=index: self.pings[index].append(self.clock.now)

    def test_synthetic_reply_exceeds_old_deadline(self):
        token = self.synthetic["token"]
        with patch("kms_stress.hardware.time.monotonic", self.clock), self.assertRaises(TimeoutError):
            self.channel.request(f"usb-status {token}", f"KMSUSBSTATUS END id={token}", timeout=1.2)

    def test_delayed_reply_reports_host_fault_without_expiring_generator_leases(self):
        collector = USBCollector(self.runner); collector.token = self.synthetic["token"] - 1
        collector.before = sample(fault=1)
        with patch("kms_stress.hardware.time.monotonic", self.clock), self.assertRaisesRegex(WorkloadStalled, "New S3 USB fault"):
            collector.poll()
        self.assertEqual(collector.samples[-1]["fault"], 2)
        self.assertAlmostEqual(self.clock.now, self.synthetic["reply_seconds"])
        for pings in self.pings:
            self.assertGreater(len(pings), 3)
            self.assertLessEqual(max(b - a for a, b in zip([0, *pings], [*pings, self.clock.now])), 0.51)

    def test_silent_uart_has_fixed_deadline_and_continues_heartbeats(self):
        self.channel.lines = TimedLines(self.clock, [])
        with patch("kms_stress.hardware.time.monotonic", self.clock), self.assertRaises(TimeoutError):
            USBCollector(self.runner).snapshot()
        self.assertAlmostEqual(self.clock.now, 8)
        self.assertGreater(len(self.pings[0]), 12)

    def test_cancel_and_reader_error_interrupt_the_extended_wait(self):
        for failure in ("cancel", "reader"):
            with self.subTest(failure=failure):
                self.setUp(); original = self.runner.service_workload
                def service():
                    if self.clock.now >= 0.5:
                        if failure == "cancel": self.runner.cancel.set()
                        else: self.channel.error = RuntimeError("UART disconnected")
                    original()
                self.runner.service_workload = service
                with patch("kms_stress.hardware.time.monotonic", self.clock), self.assertRaises(Cancelled if failure == "cancel" else RuntimeError):
                    USBCollector(self.runner).snapshot()
                self.assertLess(self.clock.now, 0.7)

    def test_stopped_sources_are_not_kept_alive(self):
        self.runner.segment_open = False
        with patch("kms_stress.hardware.time.monotonic", self.clock):
            self.runner.service_workload()
        self.assertEqual(self.pings, [[], []])


if __name__ == "__main__": unittest.main()
