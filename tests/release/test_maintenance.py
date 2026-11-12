"""Exercise release imports, isolated maintenance and both device-update transports."""
import copy
from argparse import Namespace
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from test_firmware import make_bundle
from kms_update import firmware, management, releases
from kms_update.fixture_lock import fixture_lock


def make_release(folder, product="regular"):
    document = make_bundle(folder)
    document["product"] = product
    for role, chip in (("controller", 9), ("receiver", 2)):
        record = document["roles"][role]["files"][-1]
        old = folder / record["path"]
        data = bytearray(256); data[0] = 0xE9; data[12:14] = chip.to_bytes(2, "little"); data[23] = 1
        data += hashlib.sha256(data).digest()
        path = folder / role / f"kmswitcher_{role}.bin"
        path.write_bytes(data); old.unlink(); record.update(path=path.relative_to(folder).as_posix(), size=len(data), sha256=hashlib.sha256(data).hexdigest())
        path.with_suffix(".json").write_text(json.dumps(dict(schema=1, git=document["commit"], role=role,
            chip=chip, size=len(data), sha256=record["sha256"], input_protocol=2, management_protocol=1, ota_schema=1)))
    if product == "stress":
        generator = copy.deepcopy(document["roles"]["controller"])
        generator["files"] = [item for item in generator["files"] if not item["path"].endswith("ota.bin")]
        document["roles"]["generator"] = generator
    (folder / "bundle.json").write_text(json.dumps(document))
    return document


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(); self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / "bundle"; self.bundle.mkdir(); make_release(self.bundle)
        self.archive = self.root / "release.zip"
        self.pack()

    def pack(self):
        with zipfile.ZipFile(self.archive, "w") as archive:
            for path in self.bundle.rglob("*"):
                if path.is_file(): archive.write(path, "firmware/" + path.relative_to(self.bundle).as_posix())

    def test_azure_shaped_archive_imports_and_validates_ota_metadata(self):
        destination = releases.install_archive(self.archive, "regular", cache=self.root / "cache")
        self.assertEqual(firmware.Bundle(destination).validate()["version"], "0.1.0")
        self.assertEqual(firmware.Bundle(destination).application("receiver")[1]["chip"], 2)

    def test_wrong_product_corruption_and_release_substitution_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"): releases.install_archive(self.archive, "stress", cache=self.root / "cache")
        with self.assertRaisesRegex(ValueError, "identity"):
            releases.install_archive(self.archive, "regular", expected=dict(version="9.9.9", commit="b" * 40), cache=self.root / "cache")
        with self.assertRaisesRegex(ValueError, "checksum"):
            releases.install_archive(self.archive, "regular", expected=dict(sha256="0" * 64), cache=self.root / "cache")
        (self.bundle / "receiver/kmswitcher_receiver.bin").write_bytes(b"damaged"); self.pack()
        with self.assertRaisesRegex(ValueError, "checksum"): releases.install_archive(self.archive, "regular", cache=self.root / "cache")

    def test_archive_traversal_duplicate_paths_and_links_are_rejected(self):
        for name in ("../outside", "/absolute", "C:/outside", "folder\\outside"):
            with zipfile.ZipFile(self.archive, "w") as archive: archive.writestr(name, "data")
            with self.subTest(name=name), self.assertRaises(ValueError):
                releases.install_archive(self.archive, "regular", cache=self.root / "cache")
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("File", "first"); archive.writestr("file", "second")
        with self.assertRaisesRegex(ValueError, "duplicate"): releases.install_archive(self.archive, "regular", cache=self.root / "cache")

    def test_stress_bundle_has_complete_generator_and_cannot_be_regular(self):
        make_release(self.bundle, "stress")
        bundle = firmware.Bundle(self.bundle, product="stress")
        self.assertEqual(len(bundle.files("generator")), 3)
        with self.assertRaises(ValueError): firmware.Bundle(self.bundle, product="regular")

    def test_public_feed_filters_product_and_development_releases(self):
        rows = [dict(product=product, stable=stable, url="firmware.zip", version="1.0.0", commit="a" * 40)
                for product in ("regular", "stress") for stable in (True, False)]
        with patch.object(releases, "fetch", return_value=json.dumps(dict(schema=1, releases=rows)).encode()):
            self.assertEqual(len(releases.discover("https://example.invalid/releases/index.json", "regular")), 1)
            self.assertEqual(len(releases.discover("https://example.invalid/releases/index.json", "stress", development=True)), 2)
        for url in ("http://example.invalid", "https://user:secret@example.invalid"):
            with self.assertRaises(ValueError): releases.https_url(url)


class MaintenanceTests(unittest.TestCase):
    def test_direct_usb_hides_ota_controls_and_flashes_only_the_connected_role(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QApplication, QMessageBox
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(firmware, "ports", return_value=[]), \
             patch.object(releases, "default_source", return_value=None):
            folder = Path(temporary); document = make_release(folder)
            window = launch(bundle=folder, execute=False)
            try:
                window.port.addItem("Test programming port", "not-a-device")
                window.port.setCurrentIndex(1)
                identity = dict(git=document["commit"], sha256="0" * 64, capabilities=2)
                window.method.setCurrentIndex(1)
                window.inspected(dict(devices=[dict(slot=slot, identity=identity) for slot in (0, 1, 3)]))
                window.list.item(2).setCheckState(Qt.CheckState.Checked)
                window.force.setChecked(True)
                ota_controls = (window.inspect_button, window.list, window.force,
                                window.receiver_source_label, window.receiver_source)
                for control in ota_controls:
                    self.assertFalse(control.isHidden())
                window.method.setCurrentIndex(0)
                for control in (*ota_controls, window.cache_label):
                    self.assertTrue(control.isHidden())
                for role in ("controller", "receiver"):
                    window.role.setCurrentIndex(window.role.findData(role))
                    with self.subTest(role=role), patch.object(window, "command") as run, \
                         patch.object(window, "failure") as failure, \
                         patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes) as review:
                        window.update_devices()
                        run.assert_called_once_with("flash", ["--role", role])
                        failure.assert_not_called()
                        self.assertIn(f"Target: {role}\n", review.call_args.args[2])
                window.method.setCurrentIndex(1)
                for control in ota_controls:
                    self.assertFalse(control.isHidden())
                self.assertEqual(window.list.item(2).checkState(), Qt.CheckState.Checked)
                for row in range(window.list.count()):
                    window.list.item(row).setCheckState(Qt.CheckState.Unchecked)
                with patch.object(window, "command") as run, patch.object(window, "failure") as failure:
                    window.update_devices()
                    run.assert_not_called()
                    self.assertIn("tick the devices", str(failure.call_args.args[0]))
            finally:
                window.close()

    def test_clear_log_during_operation_preserves_output_and_accepts_new_messages(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtWidgets import QApplication
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        with patch.object(firmware, "ports", return_value=[]), \
             patch.object(releases, "default_source", return_value=None):
            window = launch(execute=False)
            try:
                window.output = bytearray(b'{"devices": []}')
                window.log.appendPlainText("Previous output")
                window.set_busy(True)
                self.assertTrue(window.clear_log_button.isEnabled())
                window.clear_log_button.click()
                self.assertEqual(window.log.toPlainText(), "")
                self.assertEqual(window.output, b'{"devices": []}')
                self.assertTrue(window.busy)
                window.log.appendPlainText("Next output")
                self.assertEqual(window.log.toPlainText(), "Next output")
            finally:
                window.set_busy(False)
                window.close()

    def test_inspected_controller_is_optional_when_updating_dongles(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QApplication, QMessageBox
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(firmware, "ports", return_value=[]), \
             patch.object(releases, "default_source", return_value=None):
            folder = Path(temporary); document = make_release(folder)
            window = launch(bundle=folder, execute=False)
            try:
                identity = dict(git=document["commit"], sha256="0" * 64, capabilities=2)
                window.inspected(dict(devices=[dict(slot=slot, identity=identity) for slot in (0, 1, 3)]))
                for row in range(window.list.count()):
                    item = window.list.item(row)
                    self.assertTrue(item.flags() & Qt.ItemFlag.ItemIsUserCheckable)
                    self.assertEqual(item.checkState(), Qt.CheckState.Unchecked)
                window.list.item(2).setCheckState(Qt.CheckState.Checked)
                window.role.setCurrentIndex(1); window.method.setCurrentIndex(1)
                window.port.addItem("Test controller", "not-a-device"); window.port.setCurrentIndex(1)
                with patch.object(window, "command") as run, \
                     patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
                    window.update_devices()
                    run.assert_called_once_with("ota", ["--role", "receiver", "--slot", "3", "--speed", "conservative"])
                self.assertEqual(window.list.item(0).checkState(), Qt.CheckState.Unchecked)
            finally:
                window.close()

    def test_ota_checkboxes_choose_all_targets_regardless_of_device_dropdown(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QApplication, QMessageBox
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(firmware, "ports", return_value=[]), \
             patch.object(releases, "default_source", return_value=None):
            folder = Path(temporary); document = make_release(folder)
            window = launch(bundle=folder, execute=False)
            try:
                identity = dict(git=document["commit"], sha256="0" * 64, capabilities=2)
                window.inspected(dict(devices=[dict(slot=slot, identity=identity) for slot in (0, 1, 2, 3)] +
                                      [dict(slot=4, error="timeout/unreachable")]))
                window.method.setCurrentIndex(1)
                window.port.addItem("Test controller", "not-a-device"); window.port.setCurrentIndex(1)
                window.force.setChecked(True)
                for dropdown in (0, 1):
                    window.role.setCurrentIndex(dropdown)
                    for checked in ((1, 2, 3), (0,), (0, 1, 3)):
                        with self.subTest(dropdown=dropdown, checked=checked), \
                             patch.object(window, "command") as run, patch.object(window, "failure") as failure, \
                             patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes) as review:
                            for row in range(window.list.count()):
                                window.list.item(row).setCheckState(Qt.CheckState.Checked if row in checked else Qt.CheckState.Unchecked)
                            window.update_devices()
                            expected = ["--role", "controller" if 0 in checked else "receiver"]
                            for slot in checked:
                                if slot: expected += ["--slot", str(slot)]
                            run.assert_called_once_with("ota", expected + ["--speed", "conservative", "--force"])
                            failure.assert_not_called()
                            summary = review.call_args.args[2]
                            target_line = next(line for line in summary.splitlines() if line.startswith("Target:"))
                            self.assertEqual("controller" in target_line, 0 in checked)
                            for slot in range(1, 5):
                                self.assertEqual(f"dongle {slot}" in target_line, slot in checked)
                            if 0 in checked and len(checked) > 1:
                                self.assertIn("dongles will be updated before the controller", summary)
                with patch.object(window, "command") as run, \
                     patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No):
                    window.update_devices()
                    run.assert_not_called()
            finally:
                window.close()

    def test_fixture_testing_and_updates_cannot_overlap(self):
        with fixture_lock():
            with self.assertRaisesRegex(RuntimeError, "Another KMSwitcher"):
                with fixture_lock(): self.fail("Second hardware session acquired the lock")
        with fixture_lock(): pass

    def test_master_endpoint_uses_envelope_slot_one_and_does_not_use_radio(self):
        class Port:
            def write(self, data): self.sent = data
            def flush(self): pass
        port = Port(); controller = management.Controller(port)
        def lines(deadline):
            request = management.Frame.decode(bytes.fromhex(port.sent.decode().split()[1]))
            reply = management.Frame(request.op, 1, request.request_id, sender=42, flags=management.REPLY, data=b"ok")
            yield "KMSMASTER " + reply.encode().hex()
        controller.lines_until = lines
        self.assertEqual(controller.request(0, management.INFO, discover=True).data, b"ok")
        self.assertTrue(port.sent.startswith(b"master "))

    def test_controller_restores_exact_previous_route_and_reports_restoration_failure(self):
        from unittest.mock import Mock
        controller = Mock(); controller.peers.return_value = ([1, 3], 3)
        def update(*args): args[5](); return "updated"
        with patch.object(management, "update_one", side_effect=update):
            result = management.update_master(controller, b"image", {})
            self.assertEqual(result["result"], "updated"); controller.select.assert_called_once_with(3)
            controller.select.side_effect = OSError("disconnected")
            self.assertIn("disconnected", management.update_master(controller, b"image", {})["warnings"][0])

    def test_standalone_ui_exposes_both_receiver_methods_without_companion_startup(self):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtWidgets import QApplication
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        if os.name == "nt":
            from PySide6.QtGui import QFontDatabase, QFont
            QFontDatabase.addApplicationFont("C:/Windows/Fonts/segoeui.ttf")
            app.setFont(QFont("Segoe UI", 10))
        with tempfile.TemporaryDirectory() as temporary, patch.object(firmware, "ports", return_value=[]), patch.object(releases, "default_source", return_value=None):
            folder = Path(temporary); make_release(folder)
            window = launch(bundle=folder, execute=False); window.show(); app.processEvents()
            window.role.setCurrentIndex(1); window.method.setCurrentIndex(0)
            self.assertIn("No controller is required", window.instructions.text())
            window.method.setCurrentIndex(1)
            self.assertIn("powered and paired", window.instructions.text())
            window.tabs.setCurrentIndex(1); app.processEvents()
            screenshot = Path(__file__).resolve().parents[2] / "build/maintenance-ui.png"
            screenshot.parent.mkdir(exist_ok=True, parents=True); self.assertTrue(window.grab().save(str(screenshot)))
            window.close()


class MaintenanceCommandTests(unittest.TestCase):
    def test_ota_uses_each_selected_devices_image_and_updates_controller_last(self):
        from kms_update import __main__ as cli
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary); make_release(folder)
            bundle = firmware.Bundle(folder)
            for role, slots in (("receiver", [1, 3]), ("controller", []), ("controller", [1, 3])):
                for cached in (False, True):
                    with self.subTest(role=role, slots=slots, cached=cached), \
                         patch.object(cli, "open_controller"), patch.object(management, "Controller") as controller_type, \
                         patch.object(management, "stage_cache") as stage, \
                         patch.object(management, "update_targets", return_value=[dict(slot=slot, result="updated") for slot in slots]) as receivers, \
                         patch.object(management, "update_master", return_value=dict(slot=0, result="updated")) as master:
                        controller = controller_type.return_value
                        controller.peers.return_value = ([1, 2, 3, 4], 2)
                        controller.identity.return_value = dict(capabilities=8 if cached else 2)
                        order = Mock(); order.attach_mock(receivers, "receivers"); order.attach_mock(master, "master")
                        args = Namespace(command="ota", bundle=folder, product="regular", role=role,
                                         port="not-a-device", slot=slots, all=False, force=True)
                        result = cli.execute(args)
                        self.assertTrue(result["success"])
                        if slots:
                            image, metadata = bundle.application("receiver")
                            receivers.assert_called_once_with(controller, slots, 2, image, metadata, True, cached=cached)
                            if cached: stage.assert_called_once_with(controller, image, metadata)
                            else: stage.assert_not_called()
                        else:
                            receivers.assert_not_called(); stage.assert_not_called()
                        if role == "controller":
                            master.assert_called_once_with(controller, *bundle.application("controller"), True)
                            self.assertEqual(result["results"][-1]["slot"], 0)
                        else: master.assert_not_called()
                        if role == "controller" and slots:
                            self.assertEqual([call[0] for call in order.mock_calls], ["receivers", "master"])

    def test_mixed_update_rejects_invalid_receiver_firmware_before_opening_port(self):
        from kms_update import __main__ as cli
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary); make_release(folder)
            (folder / "receiver/kmswitcher_receiver.json").unlink()
            args = Namespace(command="ota", bundle=folder, product="regular", role="controller",
                             port="not-a-device", slot=[1], all=False, force=False)
            with patch.object(cli, "open_controller") as open_port:
                with self.assertRaises((OSError, ValueError)):
                    cli.execute(args)
                open_port.assert_not_called()
