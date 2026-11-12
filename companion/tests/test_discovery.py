"""Receiver discovery must use the Linux backend covered by our hidraw ACLs."""
import sys
import unittest
from unittest.mock import Mock, patch

from kms_companion.transport import DeviceError, Receiver, devices


def collection(path, page, usage, serial="dock1"):
    return dict(path=path, usage_page=page, usage=usage, serial_number=serial)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        # libusb returns zero usages on Linux. hidraw enumerates the individual
        # collections, including several collections sharing the same path.
        self.libusb = Mock()
        self.libusb.enumerate.return_value = [
            dict(collection(b"1-2:1.2", 0, 0), interface_number=2)]
        self.hidraw = Mock()
        self.hidraw.device.return_value.get_feature_report.return_value = b''
        self.libusb.device.return_value.get_feature_report.return_value = b''
        self.hidraw.enumerate.return_value = [
            collection(b"/dev/hidraw0", 1, 6),
            collection(b"/dev/hidraw1", 1, 2),
            collection(b"/dev/hidraw2", 0x0c, 1),
            collection(b"/dev/hidraw2", 1, 0x80),
            collection(b"/dev/hidraw2", 0xff00, 2),
            collection(b"/dev/hidraw2", 0xff00, 1)]
        self.modules = patch.dict(sys.modules, {"hid": self.libusb, "hidraw": self.hidraw})
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def test_linux_finds_only_desktop_collection_through_hidraw(self):
        with patch("kms_companion.transport.sys.platform", "linux"):
            found = devices()
        self.assertEqual(found, [collection(b"/dev/hidraw2", 0xff00, 2)])
        self.hidraw.enumerate.assert_called_once_with(0x303a, 0x4005)
        self.libusb.enumerate.assert_not_called()

    def test_linux_opens_the_same_backend_used_for_enumeration(self):
        with patch("kms_companion.transport.sys.platform", "linux"), patch.object(Receiver, "request"):
            receiver = Receiver()
            receiver.close()
        self.hidraw.device.return_value.open_path.assert_called_once_with(b"/dev/hidraw2")
        self.hidraw.device.return_value.close.assert_called_once_with()
        self.libusb.device.assert_not_called()

    def test_windows_keeps_native_hid_collection_selection(self):
        desktop = collection(b"windows-vendor-collection", 0xff00, 2)
        self.libusb.enumerate.return_value = [desktop, collection(b"keyboard", 1, 6)]
        with patch("kms_companion.transport.sys.platform", "win32"), patch.object(Receiver, "request"):
            receiver = Receiver()
            receiver.close()
        self.libusb.device.return_value.open_path.assert_called_once_with(desktop["path"])
        self.hidraw.enumerate.assert_not_called()
        self.hidraw.device.assert_not_called()

    def test_linux_does_not_accept_receiver_without_desktop_collection(self):
        self.hidraw.enumerate.return_value = [collection(b"/dev/hidraw2", 0xff00, 1)]
        with patch("kms_companion.transport.sys.platform", "linux"):
            self.assertEqual(devices(), [])
            with self.assertRaises(DeviceError):
                Receiver()
        self.hidraw.device.assert_not_called()

    def test_multiple_linux_receivers_require_explicit_serial(self):
        self.hidraw.enumerate.return_value.append(collection(b"/dev/hidraw5", 0xff00, 2, "dock2"))
        with patch("kms_companion.transport.sys.platform", "linux"), patch.object(Receiver, "request"):
            with self.assertRaises(DeviceError):
                Receiver()
            self.hidraw.device.assert_not_called()
            receiver = Receiver("dock2")
            receiver.close()
        self.hidraw.device.return_value.open_path.assert_called_once_with(b"/dev/hidraw5")

    def test_missing_linux_backend_is_not_replaced_by_libusb(self):
        with patch("kms_companion.transport.sys.platform", "linux"), patch.dict(sys.modules, {"hidraw": None}):
            with self.assertRaises(ImportError):
                devices()
        self.libusb.enumerate.assert_not_called()


if __name__ == "__main__":
    unittest.main()
