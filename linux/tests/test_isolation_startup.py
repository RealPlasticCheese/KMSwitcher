"""Exercise delayed startup against the production UART request loop."""
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import Mock, patch

from kms_stress.hardware import SerialChannel
from kms_stress.runner import Cancelled
from kms_stress.usb_isolation import IsolationRunner, read_isolation_identity
from test_delayed_usb_status import Clock, TimedLines
from test_usb_isolation import MASTER, config


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock(); self.clock.now = 0
        events = json.loads((Path(__file__).parent / 'fixtures/isolation-startup.json').read_text())['events']
        self.channel = SerialChannel.__new__(SerialChannel)
        self.channel.path, self.channel.error = 'synthetic master', None
        self.channel.command_lock = threading.Lock()
        self.channel.log = Mock(); self.channel.port = Mock()
        self.channel.lines = TimedLines(self.clock, events)
        self.channel.restarted = True; self.channel.last_boot_id = 'd3b139c1'
        self.runner = IsolationRunner(config('unused'))
        self.runner.control.master = self.channel
        self.queries = 0
        self.channel.port.write.side_effect = self.write

    def write(self, data):
        if data.strip() == b'iso info':
            self.queries += 1
            if self.queries == 2:
                self.channel.lines.events.append(dict(seconds=self.clock.now + .01,
                    line=f'KMSISO INFO 1 {MASTER} d3b139c1 bfb958b82c09 idf=v6.1 usb=1.6.0'))

    def probe(self, identity=MASTER, check=None):
        with patch('kms_stress.hardware.time.monotonic', self.clock):
            return read_isolation_identity(self.channel, identity, check or self.runner.control.check, Mock())

    def test_comparison_and_baseline_versions_are_explicitly_verified(self):
        for version in ('1.5.0', '1.6.0', '1.4.1', '1.7.0', 'invalid'):
            with self.subTest(version=version):
                self.setUp()
                self.channel.lines.events = [dict(seconds=.01,
                    line=f'KMSISO INFO 1 {MASTER} d3b139c1 test idf=v6.1 usb={version}')]
                if version in ('1.5.0', '1.6.0'):
                    self.assertEqual(self.probe()[7], 'usb=' + version)
                else:
                    with self.assertRaisesRegex(RuntimeError, 'firmware/chip'): self.probe()
                self.assertEqual(self.queries, 1)
                self.assertEqual(self.channel.port.write.call_count, 2)

    def test_trace_capability_is_explicit_and_unknown_extra_fields_are_rejected(self):
        for capability in ("trace=1", "trace=2", "other=1"):
            with self.subTest(capability=capability):
                self.setUp()
                self.channel.lines.events = [dict(seconds=.01,
                    line=f'KMSISO INFO 1 {MASTER} d3b139c1 test idf=v6.1 usb=1.6.0 {capability}')]
                if capability == "trace=1": self.assertEqual(self.probe()[8], capability)
                else:
                    with self.assertRaises(RuntimeError): self.probe()

    def test_original_single_query_loses_synthetic_startup(self):
        with patch('kms_stress.hardware.time.monotonic', self.clock), self.assertRaises(TimeoutError):
            self.channel.request('iso info', 'KMSISO INFO ', timeout=8)
        self.assertEqual(self.queries, 1)

    def test_retry_accepts_boot_then_identity_on_same_connection(self):
        reply = self.probe()
        self.assertEqual(reply[4], 'd3b139c1')
        self.assertEqual(self.queries, 2)
        self.assertLess(self.clock.now, 1.6)
        self.assertEqual([c.args[0].strip() for c in self.channel.port.write.call_args_list],
                         [b'', b'iso info', b'', b'iso info'])

    def test_silent_port_has_bounded_four_read_only_attempts(self):
        self.channel.port.write.side_effect = None
        with self.assertRaises(TimeoutError): self.probe()
        self.assertAlmostEqual(self.clock.now, 6)
        self.assertEqual(self.channel.port.write.call_count, 8)

    def test_wrong_identity_fails_without_further_queries(self):
        with self.assertRaisesRegex(RuntimeError, 'firmware/chip'): self.probe('000000000000')
        self.assertEqual(self.queries, 2)

    def test_cancel_and_reader_failure_interrupt_startup(self):
        for failure in ('cancel', 'reader'):
            with self.subTest(failure=failure):
                self.setUp()
                def check():
                    if self.clock.now >= .2:
                        if failure == 'cancel': self.runner.control.cancel.set()
                        else: self.channel.error = RuntimeError('UART disconnected')
                    self.runner.control.check()
                with self.assertRaises(Cancelled if failure == 'cancel' else RuntimeError):
                    self.probe(check=check)
                self.assertLess(self.clock.now, .4)
                self.assertEqual(self.queries, 1)

    def test_restart_guard_still_rejects_boot_after_handshake(self):
        self.probe()
        with self.assertRaisesRegex(RuntimeError, 'restarted'): self.runner.service()
