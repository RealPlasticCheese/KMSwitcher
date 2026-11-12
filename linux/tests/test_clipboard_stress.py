"""Real stress traffic pump over fake HID/radio transport; no network or files."""
from collections import defaultdict, deque
import struct
import unittest

from kms_stress.clipboard import ClipboardTraffic, decode, packet
from kms_stress.model import Config, Device
from kms_stress.runner import Runner
from kms_stress.suite import plan
from kms_stress.diagnostics import uart_fault


class Transport:
    def __init__(self):
        self.desktop = {}; self.queues = defaultdict(deque)
        self.sent = []; self.drop = False; self.corrupt = False

    def clipboard_feature(self, slot, report, payload=None):
        if report == 32:
            if payload is not None: self.desktop[slot] = payload.ljust(192, b"\0")
            return self.desktop.get(slot, bytes(192))
        if report == 34:
            message = decode(payload); self.sent.append(message)
            if not self.drop:
                if self.corrupt: payload = payload[:40] + bytes([payload[40] ^ 1]) + payload[41:]
                self.queues[message["target"]].append(payload)
            return payload
        return self.queues[slot].popleft() if self.queues[slot] else bytes(192)


class ClipboardStressTests(unittest.TestCase):
    def setUp(self):
        self.now = 100.; self.transport = Transport(); self.events = []
        self.load = ClipboardTraffic(self.transport, 3, 2, lambda *event: self.events.append(event), lambda: self.now)

    def run_for(self, seconds):
        end = self.now + seconds
        while self.now < end:
            self.load.tick(); self.now += .02

    def test_complete_retry_interrupted_receive_cancel_and_recovery(self):
        self.run_for(8)
        result = self.load.finish()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["bytes_received"], 1440)
        self.assertTrue(result["complete"] and result["cancelled"])
        self.assertGreaterEqual(result["retries"], 1)
        self.assertGreaterEqual(result["duplicates"], 1)
        self.assertEqual({p["kind"] for p in self.transport.sent}, {3, 4, 5, 6, 7})
        self.assertEqual(result["target"], 2)
        self.load.close()
        for raw in self.transport.desktop.values(): self.assertEqual(struct.unpack_from("<I", raw, 12)[0], 0)

    def test_stale_reply_cannot_advance_transfer(self):
        self.transport.queues[3].append(packet(6, 2, 3, {2: 9, 3: 10}, 1, 1))
        self.run_for(8)
        self.assertTrue(self.load.finish()["passed"])
        self.assertEqual(self.load.stale, 1)

    def test_corruption_and_complete_loss_fail_instead_of_passing(self):
        self.transport.corrupt = True
        with self.assertRaisesRegex(RuntimeError, "corrupt"): self.run_for(1)
        self.setUp(); self.transport.drop = True
        with self.assertRaisesRegex(RuntimeError, "five attempts"): self.run_for(6)
        self.assertFalse(self.load.finish()["passed"])

    def test_input_time_after_transfer_is_required_even_if_cleanup_waits(self):
        self.load.completed = self.load.interrupted = True
        self.load.recovery_since = self.load.last_tick
        self.now += 20
        self.assertFalse(self.load.finish()["passed"])

    def test_missing_helper_session_has_a_deadline(self):
        self.transport.clipboard_feature = lambda *args: bytes(192)
        with self.assertRaisesRegex(RuntimeError, "heartbeat stalled"): self.run_for(5)

    def test_preset_and_timed_loop_include_each_receiving_dongle_and_recovery(self):
        config = Config(master=Device("112233445566", uart="master"),
                        generators=[Device("AABBCCDDEE01", uart="generator")],
                        receivers=[Device("AABBCCDDEE02", slot=2), Device("AABBCCDDEE03", slot=3)],
                        preset="clipboard", rates=[25], seconds=3)
        config.validate()
        schedule = list(Runner(config).schedule())
        self.assertEqual([s[0] for s in schedule], [2, 2, 3, 3])
        self.assertEqual([s[2] for s in schedule], [15, 3, 15, 3])
        tests = plan(config)
        for i, test in enumerate(tests):
            if test.name == "clipboard":
                self.assertEqual((tests[i + 1].name, tests[i + 1].slot), ("clipboard-recovery", test.slot))
        config.receivers.pop()
        with self.assertRaisesRegex(ValueError, "two enabled"): config.validate()
        self.assertFalse(any(t.task == "clipboard" for t in plan(config)))

    def test_radio_driver_recovery_is_a_failure_in_stress_evidence(self):
        self.assertIn("Radio transmit", uart_fault("E (10) link: Radio completion missing for 01:02 age=1000ms"))
        self.assertIsNone(uart_fault("I (10) link: KMSRADIO status slot=2"))
