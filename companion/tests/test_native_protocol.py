"""Exercise Python's actual HID transport against the production C master core."""
import ctypes
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch
from kms_companion.geometry import Rect
from kms_companion.transport import Receiver, RejectedCommand

LIBRARY = Path(__file__).resolve().parents[2] / "build/desktop-tests/desktop-tests.dll"


@unittest.skipUnless(sys.platform == "win32" and LIBRARY.exists(), "Build scripts/test-desktop.py first on Windows")
class NativeProtocolTests(unittest.TestCase):
    def setUp(self):
        self.native = ctypes.CDLL(str(LIBRARY))
        self.native.kms_desktop_sim_init()
        native = self.native

        class FakeDevice:
            def open_path(self, path): pass
            def close(self): pass
            def send_feature_report(self, report):
                out = ctypes.create_string_buffer(64)
                self.result = bytes([32]) + out.raw
                if native.kms_desktop_sim_exchange(2, report[1:], out):
                    self.result = bytes([32]) + out.raw
                return len(report)
            def get_feature_report(self, ident, size):
                if ident == 33: return b''  # This fixture models the legacy S2 status capability.
                return self.result.ljust(size, b"\0")

        fake = types.SimpleNamespace(device=FakeDevice, enumerate=lambda *_: [dict(usage_page=0xff00, usage=2,
            path=b"fake", serial_number="dock2")])
        self.hid = patch.dict(sys.modules, {"hid": fake})
        self.hid.start()
        self.receiver = Receiver()

    def tearDown(self):
        self.receiver.close(); self.hid.stop()

    def test_configuration_roundtrip_and_stale_save(self):
        config, revision = self.receiver.config()
        config["stations"][0].update(x=-1366, y=-768)
        config["stations"][1].update(width=3840, height=2160)
        config.update(gap_tolerance=2**32 - 1, overlap_tolerance=0)
        new_revision = self.receiver.save_config(config, revision, refresh_joins=True)
        actual, actual_revision = self.receiver.config()
        self.assertEqual(actual, config)
        self.assertEqual(actual_revision, new_revision)
        with self.assertRaises(RejectedCommand):
            self.receiver.save_config(config, revision)

    def test_monitor_geometry_and_enable_roundtrip(self):
        self.receiver.screens([Rect(-100, 0, 100, 100), Rect(0, 0, 200, 150)], 42)
        self.receiver.heartbeat(True, True)
        peer = self.receiver.peers(include_joins=True)[1]
        self.assertEqual(len(peer["screens"]), 2)
        self.assertIn("crossings", peer)
        self.assertTrue(peer["eligible"])
        self.receiver.heartbeat(False, False)
        self.assertFalse(self.receiver.peers()[1]["eligible"])


if __name__ == "__main__":
    unittest.main()
