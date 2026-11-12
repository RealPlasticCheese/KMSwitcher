"""Discovery and callback regressions, without opening any hardware."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from kms_stress import __version__
from kms_stress.hardware import FirmwareIdentity
from kms_stress.model import Device


class DashboardTests(unittest.TestCase):
    def setUp(self):
        try:
            import tkinter as tk
            self.root = tk.Tk()
            self.root.withdraw()
        except Exception as error:
            self.skipTest(f"Tk display unavailable: {error}")
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.root.destroy)
        from kms_stress.gui import Dashboard
        self.dashboard = Dashboard(self.root, Path(self.directory.name) / "profile.json")

    def test_identity_survives_failed_pairing_query(self):
        device = Device("AABBCCDDEEFF", "Master", "bridge")
        with patch("kms_stress.discovery.discover_serial", return_value=([("master", device)], [])), \
             patch("kms_stress.discovery.receiver_nodes", return_value={}), \
             patch("kms_stress.discovery.SerialChannel"), \
             patch("kms_stress.discovery.read_identity", return_value=FirmwareIdentity("master", device.identity)), \
             patch("kms_stress.discovery.peers", side_effect=TimeoutError("inventory failed")):
            self.dashboard.discover()
            self.dashboard.worker.join(timeout=3)
        self.assertFalse(self.dashboard.busy())
        self.dashboard.process_messages()
        self.assertEqual(self.dashboard.config.master.identity, device.identity)
        self.assertEqual(self.dashboard.config.master.uart, "bridge")
        self.assertIn("pairing inventory unavailable", self.dashboard.output.get("1.0", "end"))

    def test_ui_callback_error_cannot_stop_future_message_polls(self):
        self.dashboard.messages.put(("discovered", self.dashboard.config))
        with patch.object(self.root, "after") as after, patch.object(self.dashboard, "refresh", side_effect=ValueError("example callback failure")):
            with self.assertRaises(ValueError): self.dashboard.poll()
            after.assert_called_once_with(100, self.dashboard.poll)
        self.dashboard.callback_error(ValueError, ValueError("example callback failure"), None)
        self.dashboard.messages.put(("text", "Next message still works"))
        self.dashboard.process_messages()
        output = self.dashboard.output.get("1.0", "end")
        self.assertIn("example callback failure", output)
        self.assertIn("Next message still works", output)

    def test_copy_diagnostics_includes_version_and_callback_error(self):
        self.dashboard.callback_error(ValueError, ValueError("support evidence"), None)
        with patch.object(self.root, "clipboard_clear"), patch.object(self.root, "clipboard_append") as append:
            self.dashboard.copy_diagnostics()
        text = append.call_args.args[0]
        self.assertIn(f"KMSwitcher hardware test {__version__}", text)
        self.assertIn("ValueError: support evidence", text)

    def test_bulk_enable_and_disable_six_slaves_preserves_selection(self):
        self.dashboard.config.receivers = [Device(f"AABBCCDDEE{slot:02X}", slot=slot, enabled=slot % 2 == 0) for slot in range(1, 7)]
        self.dashboard.config.generators = [Device("112233445566", enabled=False)]
        self.dashboard.refresh()
        selected = ("0",) + tuple(str(i) for i in range(2, 8))  # Master plus six slaves; generator stays unselected.
        self.dashboard.devices.selection_set(selected)
        self.dashboard.set_enabled(True)
        self.assertTrue(all(d.enabled for d in self.dashboard.config.receivers))
        self.assertFalse(self.dashboard.config.generators[0].enabled)
        self.assertEqual(self.dashboard.devices.selection(), selected)
        self.dashboard.set_enabled(False)
        self.assertFalse(any(d.enabled for d in self.dashboard.config.receivers))
        self.assertTrue(self.dashboard.config.master.enabled)
        self.assertEqual(self.dashboard.devices.selection(), selected)
        self.assertTrue(all(self.dashboard.devices.set(str(i), "enabled") == "no" for i in range(2, 8)))

    def test_select_all_does_not_allow_single_row_removal(self):
        self.dashboard.config.receivers = [Device("AABBCCDDEEFF", slot=3)]
        self.dashboard.refresh()
        self.assertEqual(self.dashboard.select_all(), "break")
        self.dashboard.update_selection_actions()
        self.assertEqual(len(self.dashboard.devices.selection()), 2)
        self.assertIsNone(self.dashboard.selected())
        self.assertTrue(all(str(button['state']) == "disabled" for button in self.dashboard.single_selection_buttons))
        self.dashboard.remove()
        self.assertEqual(len(self.dashboard.config.receivers), 1)


if __name__ == "__main__": unittest.main()
