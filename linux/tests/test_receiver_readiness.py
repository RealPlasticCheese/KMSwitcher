"""Selection semantics and cleanup through a stateful paired-receiver fixture."""
import unittest
from unittest.mock import Mock, patch

from kms_stress.model import Config, Device
from kms_stress.runner import Runner


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.runner = Runner(Config(receivers=[Device("AABBCCDDEE03", slot=3), Device("AABBCCDDEE06", slot=6)]))
        self.current = 3
        self.paired = {3: "11111111", 6: "22222222"}
        self.usb_unready = set()
        self.accept_unpaired = self.reject_usb_unready = False
        self.runner.wait = Mock()
        self.runner.master = Mock(request=Mock(side_effect=self.request))
        self.runner.capture = Mock(feature=Mock(side_effect=self.feature))
        peers = patch("kms_stress.runner.peers", side_effect=lambda _: ({}, self.paired, self.current, []))
        peers.start(); self.addCleanup(peers.stop)

    def feature(self, slot, operation, value):
        self.assertEqual(operation, 6)
        if value: self.usb_unready.add(slot)
        else: self.usb_unready.discard(slot)

    def request(self, command, prefix):
        slot = int(command.split()[1])
        if ((slot not in self.paired and not self.accept_unpaired) or
                (slot in self.usb_unready and self.reject_usb_unready)):
            raise RuntimeError("selection unavailable")
        self.current = slot
        return [prefix]

    def test_reachable_usb_not_ready_is_selected_and_unpaired_target_is_rejected(self):
        result = self.runner.availability_check(3)
        self.assertTrue(result["passed"])
        self.assertEqual(result["current"], 6)
        self.assertTrue(result["unpaired_target"]["rejected"])
        self.assertEqual(self.current, 3)
        self.assertFalse(self.usb_unready)

    def test_selection_failure_still_restores_usb_readiness_and_original_route(self):
        self.reject_usb_unready = True
        with self.assertRaisesRegex(RuntimeError, "selection unavailable"):
            self.runner.availability_check(3)
        self.assertEqual(self.current, 3)
        self.assertFalse(self.usb_unready)

    def test_accepting_an_unpaired_slot_fails_and_restores_original_route(self):
        self.accept_unpaired = True
        result = self.runner.availability_check(3)
        self.assertFalse(result["passed"])
        self.assertFalse(result["unpaired_target"]["passed"])
        self.assertEqual(self.current, 3)

    def test_full_pairing_table_records_negative_check_as_unavailable_coverage(self):
        self.paired = dict.fromkeys(range(1, 8), "11111111")
        result = self.runner.availability_check(3)
        self.assertTrue(result["passed"])
        self.assertIn("skipped", result["unpaired_target"])


if __name__ == "__main__": unittest.main()
