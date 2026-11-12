"""Desk-editor acceptance tests with real Qt items, mouse events and file writes."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from concurrent.futures import Future
from unittest.mock import patch

from kms_companion.protocol import default_config
from kms_companion.ui import validate_document

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


class DocumentTests(unittest.TestCase):
    def test_invalid_imports_are_rejected_before_controls_change(self):
        good = dict(config=default_config(), revision=3)
        for bad in (None, [], {}, dict(config=[]), dict(config={}, revision=3),
                    dict(config=good["config"], revision=True),
                    dict(config=good["config"], revision=-1),
                    dict(config=good["config"], revision=2**32)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                validate_document(bad)
        validated = validate_document(good)
        validated["config"]["stations"][0]["x"] = 50
        self.assertEqual(good["config"]["stations"][0]["x"], 0)


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "Optional UI dependencies not installed")
class LayoutUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])
        if os.name == "nt":
            from PySide6.QtGui import QFontDatabase, QFont
            QFontDatabase.addApplicationFont("C:/Windows/Fonts/segoeui.ttf")
            cls.app.setFont(QFont("Segoe UI", 10))

    def setUp(self):
        from kms_companion.ui import launch
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.calls = []
        self.doc = dict(config=default_config(), revision=10)
        self.peers = [dict(slot=1, eligible=True, ready=True, enabled=True, session=1, age_ms=1,
                           screens=[dict(x=0, y=0, width=1920, height=1080),
                                    dict(x=1920, y=0, width=2560, height=1440)])]
        self.ipc = patch("kms_companion.ipc.call", side_effect=self.call)
        self.ipc.start(); self.addCleanup(self.ipc.stop)
        with patch("kms_companion.cli.startup_enabled", return_value=False):
            self.window = launch(execute=False)
        self.window.timer.stop()
        self.addCleanup(self.close_window)
        self.drain()

    def call(self, command, **args):
        self.calls.append((command, args))
        if command == "status":
            return dict(connected=True, slot=1, selected=1, enabled=True, revision=self.doc["revision"])
        if command == "config-get":
            return copy.deepcopy(self.doc)
        if command == "config-set":
            if args["revision"] != self.doc["revision"]:
                raise RuntimeError("configuration changed; reload before saving")
            self.doc = dict(config=copy.deepcopy(args["config"]), revision=args["revision"] + 1)
            return copy.deepcopy(self.doc)
        if command == "peers":
            return copy.deepcopy(self.peers)
        if command == "clipboard-status":
            return dict(items=[], peers=[])
        return args

    def close_window(self):
        self.window.timer.stop()
        self.window.pool.shutdown(wait=True)
        self.window.transfer_pool.shutdown(wait=True)
        self.window.device_stop.set()
        self.window.device_pool.shutdown(wait=True, cancel_futures=True)
        self.window.tray.hide(); self.window.hide()
        self.window.deleteLater(); self.app.processEvents()

    def drain(self):
        deadline = time.monotonic() + 3
        while self.window.pending and time.monotonic() < deadline:
            self.window.tick(); self.app.processEvents(); time.sleep(.01)
        self.assertFalse(self.window.pending)
        self.app.processEvents()

    def test_clipboard_save_reveals_only_after_success_and_retains_location_action(self):
        from PySide6.QtCore import QEvent
        from PySide6.QtWidgets import QPushButton
        item = dict(id=51, direction="receive", state="Ready", kind="files", prepared=True)
        self.window.clipboard_state = dict(items=[item])
        self.window.show_transfers()

        def buttons():
            return {b.text(): b for b in self.window.transfer_table.cellWidget(0, 6).findChildren(QPushButton)}

        self.assertNotIn("Open location", buttons())
        with patch("PySide6.QtWidgets.QFileDialog.getExistingDirectory", return_value=self.directory.name), \
             patch.object(self.window, "submit") as submit, \
             patch("kms_companion.file_manager.reveal_path") as reveal:
            buttons()["Save to…"].click()
            reveal.assert_not_called()
            self.assertNotIn("Open location", buttons())
            self.assertEqual(submit.call_args.args, ("clipboard-save",))
            self.assertEqual(submit.call_args.kwargs["id"], 51)
            future = Future()
            future.kms_command = "clipboard-save"
            self.window.pending.append((future, submit.call_args.kwargs["callback"]))
            path = str(Path(self.directory.name) / "KMSwitcher-00000033")
            future.set_result(dict(path=path))
            self.drain()
            reveal.assert_called_once_with(path)
            self.assertEqual(self.window.note.text(), "Saved to " + path)
            self.assertIn("Open location", buttons())
            actions = self.window.transfer_table.cellWidget(0, 6)
            self.app.processEvents()
            self.app.sendPostedEvents(None, QEvent.Type.DeferredDelete)
            self.assertGreaterEqual(actions.width(), actions.layout().minimumSize().width())
            preview = Path(__file__).resolve().parents[2] / "build/clipboard-saved-location-ui.png"
            preview.parent.mkdir(parents=True, exist_ok=True)
            self.assertTrue(self.window.transfer_dialog.grab().save(str(preview)))
            self.window.refresh_transfers()
            self.assertIs(self.window.transfer_table.cellWidget(0, 6), actions)
            self.window.transfer_dialog.hide()
            self.window.show_transfers()
            buttons()["Open location"].click()
            self.drain()
            self.assertEqual(reveal.call_count, 2)
            reveal.assert_called_with(path)

    def test_clipboard_save_cancellation_and_failure_do_not_reveal_or_enable_location(self):
        from PySide6.QtWidgets import QPushButton
        self.window.clipboard_state = dict(items=[dict(id=51, direction="receive", state="Ready", kind="files")])
        self.window.show_transfers()
        with patch("PySide6.QtWidgets.QFileDialog.getExistingDirectory", return_value=""), \
             patch.object(self.window, "submit") as submit:
            self.window.save_received_files(51)
            submit.assert_not_called()
        with patch("PySide6.QtWidgets.QFileDialog.getExistingDirectory", return_value=self.directory.name), \
             patch.object(self.window, "submit") as submit, \
             patch("kms_companion.file_manager.reveal_path") as reveal, \
             patch.object(self.window.tray, "showMessage") as notify:
            self.window.save_received_files(51)
            future = Future()
            future.kms_command = "clipboard-save"
            self.window.pending.append((future, submit.call_args.kwargs["callback"]))
            future.set_exception(OSError("Disk full"))
            self.drain()
            reveal.assert_not_called()
            notify.assert_called_once_with("Clipboard", "Disk full")
            buttons = self.window.transfer_table.cellWidget(0, 6).findChildren(QPushButton)
            self.assertNotIn("Open location", [b.text() for b in buttons])

    def test_saved_location_action_is_restored_from_helper_and_reports_missing_folder(self):
        from PySide6.QtWidgets import QPushButton
        self.window.show_transfers()
        item = dict(id=52, direction="receive", state="Ready", kind="files", saved_path="previous save")
        self.window.show_clipboard_status(dict(items=[item]))
        button = next(b for b in self.window.transfer_table.cellWidget(0, 6).findChildren(QPushButton)
                      if b.text() == "Open location")
        with patch("kms_companion.file_manager.reveal_path", side_effect=FileNotFoundError("Folder was removed")), \
             patch.object(self.window.tray, "showMessage") as notify:
            button.click()
            self.drain()
            notify.assert_called_once_with("Clipboard", "Folder was removed")
        self.assertEqual(item["saved_path"], "previous save")

    def test_pointer_parking_is_local_and_status_refresh_does_not_save_it(self):
        parking = self.window.pointer_parking
        self.assertEqual([parking.itemText(i) for i in range(parking.count())],
                         ["Bottom right", "Bottom left", "Top right", "Top left", "Top centre", "Bottom centre", "Left centre", "Right centre", "Disabled"])
        self.assertEqual(parking.currentData(), "bottom-right")
        from kms_companion.geometry import POINTER_PARKING_MODES
        for mode in POINTER_PARKING_MODES:
            with patch.object(self.window, "submit") as submit:
                self.window.show_status(dict(pointer_parking=mode))
                self.assertEqual(parking.currentData(), mode)
                self.assertFalse(any(call.args[0] == "pointer-parking" for call in submit.call_args_list))
                parking.activated.emit(parking.currentIndex())
                submit.assert_any_call("pointer-parking", mode=mode)
        self.assertFalse(self.window.dirty)

    def test_settings_share_tabs_and_navigation_keeps_layout_edits(self):
        self.assertEqual([self.window.tabs.tabText(i) for i in range(self.window.tabs.count())],
                         ["General", "Desk layout", "Clipboard", "Transfers", "Devices", "Radio",
                          "Diagnostics", "Shortcuts"])
        self.add_station(0)
        before = self.window.current_document()
        self.window.clipboard_preferences()
        self.assertEqual(self.window.tabs.tabText(self.window.tabs.currentIndex()), "Clipboard")
        self.window.show_transfers()
        self.assertEqual(self.window.tabs.tabText(self.window.tabs.currentIndex()), "Transfers")
        self.assertEqual(self.window.current_document(), before)
        self.assertTrue(self.window.dirty)
        self.assertFalse(self.window.transfer_dialog.isWindow())

    def test_receiving_policies_dispatch_each_type_once_and_manual_keeps_clipboard(self):
        from kms_companion.clipboard_preferences import DEFAULT_RECEIVING
        kinds = ("text", "rich", "image", "files")
        items = [dict(id=i + 100, direction="receive", state="Ready", content_type=kind,
                      kind="files" if kind == "files" else "content") for i, kind in enumerate(kinds)]
        state = dict(items=items, settings=dict(receiving=DEFAULT_RECEIVING))
        def accept(identifier):
            self.window.clipboard_applied.add(identifier)
        with patch.object(self.window, "accept_clipboard", side_effect=accept) as publish, \
             patch.object(self.window, "save_received_files") as save:
            self.window.show_clipboard_status(state)
            self.window.show_clipboard_status(state)
            self.assertEqual([c.args[0] for c in publish.call_args_list], [100, 101])
            save.assert_not_called()
            self.window.clipboard_applied.clear()
            state["settings"] = dict(receiving={kind: "save" for kind in kinds}, save_folder=self.directory.name)
            self.window.show_clipboard_status(state)
            self.window.show_clipboard_status(state)
            self.assertEqual(save.call_count, 4)
            for item in items:
                save.assert_any_call(item["id"], parent=self.directory.name, automatic=True)
            publish.reset_mock(); save.reset_mock()
            self.window.clipboard_applied.clear()
            state["settings"] = dict(receiving={kind: "manual" for kind in kinds})
            self.window.show_clipboard_status(state)
            publish.assert_not_called(); save.assert_not_called()

    def test_all_received_types_offer_save_including_already_published_items(self):
        from PySide6.QtWidgets import QPushButton
        for kind in ("text", "rich", "image", "files"):
            for state in ("Ready", "On clipboard", "Saved"):
                item = dict(id=80, direction="receive", kind=kind, content_type=kind, state=state, available=True)
                self.window.clipboard_state = dict(items=[item])
                self.window.refresh_transfers()
                actions = self.window.transfer_table.cellWidget(0, 6)
                self.assertIn("Save to…", [b.text() for b in actions.findChildren(QPushButton)])
        item["available"] = False
        self.window.refresh_transfers()
        actions = self.window.transfer_table.cellWidget(0, 6)
        self.assertNotIn("Save to…", [b.text() for b in actions.findChildren(QPushButton)])

    def test_view_and_open_folder_use_verified_local_paths(self):
        from PySide6.QtGui import QDesktopServices
        from PySide6.QtWidgets import QPushButton

        folder = Path(self.directory.name)
        self.window.clipboard_state = dict(items=[dict(id=80, direction="receive", kind="files",
                                                      state="On clipboard", available=True)])
        self.window.refresh_transfers()
        actions = self.window.transfer_table.cellWidget(0, 6)
        buttons = {button.text(): button for button in actions.findChildren(QPushButton)}
        for action, name, expected in (("View", "example.txt", folder / "example.txt"),
                                       ("View", "example.exe", folder),
                                       ("Open folder", "example.txt", folder)):
            with self.subTest(action=action, name=name), patch.object(self.window, "submit") as submit, \
                 patch.object(QDesktopServices, "openUrl", return_value=True) as open_url:
                path = folder / name
                path.touch()
                buttons[action].click()
                self.assertEqual(submit.call_args.args, ("clipboard-inspect",))
                self.assertEqual(submit.call_args.kwargs["id"], 80)
                submit.call_args.kwargs["callback"](dict(path=str(path), folder=str(folder), file_transfer=True))
                self.assertEqual(Path(open_url.call_args.args[0].toLocalFile()), expected)

    def test_failed_automatic_save_is_not_repeated_and_can_be_retried_manually(self):
        item = dict(id=90, direction="receive", state="Ready", content_type="image", kind="content")
        state = dict(items=[item], settings=dict(receiving={"image": "save"}, save_folder=self.directory.name))
        with patch("kms_companion.ipc.call", side_effect=OSError("Disk full")), \
             patch.object(self.window.tray, "showMessage") as notify:
            self.window.show_clipboard_status(state)
            self.drain()
            notify.assert_called_once_with("Clipboard", "Disk full")
        self.assertNotIn(90, self.window.clipboard_processing)
        with patch.object(self.window, "save_received_files") as save:
            self.window.show_clipboard_status(state)
            save.assert_not_called()
        with patch("PySide6.QtWidgets.QFileDialog.getExistingDirectory", return_value=self.directory.name), \
             patch("kms_companion.ipc.call", return_value=dict(path="saved.png")) as call, \
             patch.object(self.window, "open_saved_location") as reveal:
            self.window.save_received_files(90)
            self.drain()
            call.assert_any_call("clipboard-save", id=90, parent=self.directory.name)
            self.assertEqual(sum(c.args[0] == "clipboard-save" for c in call.call_args_list), 1)
            reveal.assert_called_once_with("saved.png")

    def test_clipboard_settings_validate_save_folder_and_polling_preserves_edits(self):
        panel = self.window.clipboard_panel
        panel.name.setText("My edited name")
        panel.receiving["rich"].setCurrentIndex(panel.receiving["rich"].findData("save"))
        with patch.object(panel, "submit") as submit:
            panel.commit()
            submit.assert_not_called()
            self.assertIn("folder", panel.message.text())
            self.window.show_clipboard_status(dict(items=[], settings=dict(name="Old name", mode="dongle", max_mb=32)))
            self.assertEqual(panel.name.text(), "My edited name")
            panel.folder.setText(self.directory.name)
            panel.commit()
            self.assertEqual(submit.call_args.args, ("clipboard-settings",))
            settings = submit.call_args.kwargs["settings"]
            self.assertEqual(settings["receiving"]["rich"], "save")
            self.assertEqual(settings["save_folder"], self.directory.name)
            panel.name.setText("Newer edit while saving")
            submit.call_args.kwargs["callback"](settings)
            self.assertTrue(panel.dirty)
            self.assertEqual(panel.name.text(), "Newer edit while saving")

    def test_clipboard_arrival_checkboxes_cover_each_receive_policy(self):
        panel = self.window.clipboard_panel
        kinds = ("text", "rich", "image", "files")
        self.assertTrue(all(panel.arrival_popups[kind].isChecked() for kind in kinds))
        panel.arrival_popups["rich"].setChecked(False)
        panel.arrival_popups["image"].setChecked(False)
        settings = panel.values()
        settings.update(receiving=dict(text="clipboard", rich="save", image="manual", files="manual"),
                        save_folder=self.directory.name)
        items = [dict(id=index, slot=index + 1, direction="receive", state="Ready", content_type=kind)
                 for index, kind in enumerate(kinds, 1)]
        state = dict(items=items, settings=settings)

        def call(command, **args):
            if command == "clipboard-save":
                return dict(path="saved.html")
            if command == "clipboard-status":
                return state
            return self.call(command, **args)

        with patch("kms_companion.ipc.call", side_effect=call), \
             patch("kms_companion.clipboard_ui.publish_clipboard"), \
             patch.object(self.window.tray, "showMessage") as notify:
            self.window.show_clipboard_status(state)
            self.drain()
            self.window.show_clipboard_status(state)
            self.window.accept_clipboard(items[-1]["id"])
            self.drain()
            self.assertEqual([popup.args[0] for popup in notify.call_args_list],
                             ["Clipboard: Ready · Dongle 5", "Clipboard received"])
            self.assertIn(("clipboard-content", dict(id=1)), self.calls)

    def test_clipboard_popup_opens_transfers_and_reveals_its_item(self):
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QPushButton
        items = [dict(id=index, direction="send", slot=1, state="Sending") for index in range(20)]
        transfer = dict(id=20, direction="receive", slot=2, state="Ready", kind="image")
        items.insert(0, transfer)
        self.window.hide()
        with patch.object(self.window.tray, "showMessage") as notify, patch.object(self.window, "submit") as submit:
            self.window.show_clipboard_status(dict(items=items))
            notify.assert_called_once()
            self.window.tray.messageClicked.emit()
            self.app.processEvents()
            self.assertTrue(self.window.isVisible())
            self.assertEqual(self.window.tabs.currentWidget(), self.window.transfer_dialog)
            table = self.window.transfer_table
            selected = table.item(20, 0)
            self.assertEqual(selected.data(Qt.ItemDataRole.UserRole), transfer["id"])
            self.assertTrue(selected.isSelected())
            self.assertGreater(table.verticalScrollBar().value(), 0)
            self.assertTrue(table.viewport().rect().contains(table.visualItemRect(selected)))
            buttons = table.cellWidget(20, 6).findChildren(QPushButton)
            self.assertEqual([button.text() for button in buttons], ["Put on clipboard", "Save to…", "View", "Open folder", "Dismiss"])
            submit.assert_not_called()

            items.append(dict(id=21, direction="send", slot=1, state="Sending"))
            self.window.show_clipboard_status(dict(items=items))
            self.assertEqual(table.currentRow(), 21)
            self.assertEqual(table.selectedItems()[0].data(Qt.ItemDataRole.UserRole), transfer["id"])

            self.window.show_clipboard_status(dict(items=items[1:]))
            self.assertFalse(table.selectedItems())

    def test_unrelated_tray_popup_does_not_reuse_clipboard_target(self):
        transfer = dict(id=1, direction="receive", slot=2, state="Ready", kind="image")
        self.window.hide()
        with patch.object(self.window.tray, "showMessage"):
            self.window.show_clipboard_status(dict(items=[transfer]))
            self.window.show_tray_message("KMSwitcher recovery", "Recovered")
            self.window.tray.messageClicked.emit()
        self.assertFalse(self.window.isVisible())

    def test_automatic_copy_tray_listener_submits_capture(self):
        from pathlib import Path
        settings = dict(automatic_send={"text": "all", "rich": "selected"}, max_mb=256)
        self.window.show_clipboard_status(dict(settings=settings, ready_session=123))
        with patch("kms_companion.agent.config_directory", return_value=Path(self.directory.name)):
            self.app.clipboard().setText("send from clipboard change")
            self.assertTrue(self.window.clipboard_auto_timer.isActive())
            self.window.clipboard_auto_timer.stop()
            self.window.send_automatic_clipboard()
            self.drain()
            args = next(args for command, args in self.calls if command == "clipboard-auto-send")
            self.assertEqual(args["source_session"], 123)
            self.assertEqual((Path(args["capture"]).parent / "format-0").read_text(), "send from clipboard change")
        self.window.clipboard_auto_timer.stop()

    def test_per_type_automatic_modes_and_compact_destination_checkboxes(self):
        from PySide6.QtWidgets import QCheckBox, QLabel
        from kms_companion.clipboard_service import TransferService
        panel = self.window.clipboard_panel
        settings = dict(panel.values(), automatic_send="selected")
        saved = TransferService.validate_settings(settings)
        self.assertEqual(set(saved["automatic_send"].values()), {"selected"})
        panel.load(settings)
        for kind in ("image", "files"):
            combo = panel.automatic[kind]
            combo.setCurrentIndex(combo.findData("off"))
        self.assertEqual(panel.values()["automatic_send"],
                         dict(text="selected", rich="selected", image="off", files="off"))
        self.assertFalse(panel.targets.isHidden())
        self.assertIsInstance(panel.target_row.itemAt(0).widget(), QLabel)

        identifier = "a" * 32
        panel.show_peers([dict(slot=2, name="Older laptop"), dict(slot=4, name="Desktop", machine_id=identifier)],
                         {2, 3, 4})
        boxes = [panel.target_row.itemAt(i).widget() for i in range(3)]
        self.assertTrue(all(isinstance(box, QCheckBox) for box in boxes))
        self.assertEqual([(box.text(), box.isEnabled()) for box in boxes],
                         [("Desktop (Dongle 4)", True), ("Older laptop (Dongle 2)", False), ("Dongle 3", False)])
        boxes[0].setChecked(True)
        panel.show_peers([dict(slot=6, name="Desktop", machine_id=identifier)], {2, 3, 6})
        self.assertTrue(panel.target_row.itemAt(0).widget().isChecked())
        self.assertEqual(panel.values()["automatic_targets"], {identifier: "Desktop"})

    def test_clipboard_cancel_click_survives_a_progress_refresh(self):
        from PySide6.QtCore import Qt
        from PySide6.QtTest import QTest
        from PySide6.QtWidgets import QPushButton
        item = dict(id=51, direction="send", slot=2, state="Sending", progress=160, size=1024)
        self.window.clipboard_state = dict(items=[item], peers=[])
        self.window.show_transfers()
        self.app.processEvents()
        actions = self.window.transfer_table.cellWidget(0, 6)
        cancel = next(button for button in actions.findChildren(QPushButton) if button.text() == "Cancel")
        with patch.object(self.window, "submit") as submit:
            QTest.mousePress(cancel, Qt.MouseButton.LeftButton)
            item["progress"] += 160
            self.window.refresh_transfers()
            self.assertIs(self.window.transfer_table.cellWidget(0, 6), actions)
            QTest.mouseRelease(cancel, Qt.MouseButton.LeftButton)
            submit.assert_called_once_with("clipboard-cancel", id=51)
        self.window.transfer_dialog.hide()

    def test_failed_incoming_transfer_keeps_a_cancel_button(self):
        from PySide6.QtWidgets import QPushButton
        self.window.clipboard_state = dict(items=[dict(id=52, direction="receive", slot=2,
            state="Failed", cancellable=True, progress=160, size=1024)], peers=[])
        self.window.show_transfers()
        actions = self.window.transfer_table.cellWidget(0, 6)
        self.assertEqual([button.text() for button in actions.findChildren(QPushButton)], ["Cancel"])
        self.window.transfer_dialog.hide()

    def add_station(self, row):
        self.window.table.cellWidget(row, 1).click()
        self.app.processEvents()

    def edit_station(self, row, **changes):
        # Exercise imported/saved dimensions through the editor's placement model.
        self.window.stations[row].update(changes)
        self.window.mark_dirty()
        self.window.draw_layout()
        self.app.processEvents()

    def export_to(self, path):
        with patch("PySide6.QtWidgets.QFileDialog.getSaveFileName", return_value=(str(path), "JSON (*.json)")):
            self.window.export_config()

    def import_from(self, path):
        with patch("PySide6.QtWidgets.QFileDialog.getOpenFileName", return_value=(str(path), "JSON (*.json)")):
            self.window.import_config()

    def test_adding_all_seven_dongles_places_each_in_free_space(self):
        for i in range(7):
            self.add_station(i)
        groups = list(self.window.groups.values())
        for i, group in enumerate(groups):
            self.assertGreater(group.boundingRect().width(), 0)
            for other in groups[i + 1:]:
                self.assertFalse(group.sceneBoundingRect().intersects(other.sceneBoundingRect()))
        self.assertTrue(self.window.dirty)
        self.window.view.fit_layout()
        output = Path(__file__).resolve().parents[2] / "build/desk-layout-seven-dongles.png"
        output.parent.mkdir(parents=True, exist_ok=True)
        self.assertTrue(self.window.grab().save(str(output)))

    def test_real_mouse_drag_moves_both_monitors_and_updates_coordinates(self):
        from PySide6.QtCore import QPointF, Qt
        from PySide6.QtTest import QTest
        self.add_station(0)
        group = self.window.groups[0]
        original_bounds = group.sceneBoundingRect()
        start = self.window.view.mapFromScene(group.mapToScene(QPointF(2200, 400)))
        finish = start + QPointF(-65, 45).toPoint()
        viewport = self.window.view.viewport()
        QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
        QTest.mouseMove(viewport, finish, delay=20)
        self.assertIsNotNone(self.window.scene.mouseGrabberItem())
        self.assertNotEqual(group.pos(), QPointF(0, 0))
        before_refresh = group.pos()
        updated = copy.deepcopy(self.peers)
        updated[0]["eligible"] = False
        self.window.loaded_peers(updated, 2)
        self.assertIs(self.window.groups[0], group)
        self.assertEqual(group.pos(), before_refresh)
        QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=finish)
        self.app.processEvents()
        group = self.window.groups[0]
        self.assertEqual(group.pos().x(), self.window.current_document()["config"]["stations"][0]["x"])
        self.assertEqual(group.pos().y(), self.window.current_document()["config"]["stations"][0]["y"])
        self.assertAlmostEqual(group.sceneBoundingRect().width(), original_bounds.width())
        self.assertEqual(self.window.topology_revision, 2)
        self.assertTrue(self.window.dirty)

    def test_mouse_drag_snaps_edges_in_all_four_directions_and_placement_stays_exact(self):
        from PySide6.QtCore import QPointF, Qt
        from PySide6.QtTest import QTest
        self.window.loaded_peers([])
        for initial, proposed, expected in (((1100, 0), (810, 10), (800, 0)),
                                            ((-1100, 0), (-810, 10), (-800, 0)),
                                            ((0, 700), (10, 460), (0, 450)),
                                            ((0, -700), (10, -460), (0, -450))):
            with self.subTest(direction=expected):
                doc = copy.deepcopy(self.doc)
                for i, pos in enumerate(((0, 0), initial)):
                    doc["config"]["stations"][i].update(configured=True, x=pos[0], y=pos[1], width=800, height=450)
                self.window.loaded(doc); self.app.processEvents()
                group = self.window.groups[1]
                start = self.window.view.mapFromScene(group.mapToScene(QPointF(200, 150)))
                finish = self.window.view.mapFromScene(QPointF(proposed[0] + 200, proposed[1] + 150))
                viewport = self.window.view.viewport()
                scale = self.window.view.transform().m11()
                QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
                QTest.mouseMove(viewport, finish, delay=20)
                self.assertEqual(self.window.view.transform().m11(), scale)
                QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=finish)
                self.app.processEvents()
                self.assertEqual((group.pos().x(), group.pos().y()), expected)
                self.window.groups[1].setX(proposed[0])
                self.window.groups[1].setY(proposed[1])
                self.assertEqual((group.pos().x(), group.pos().y()), proposed)

    def test_adding_dongles_scales_the_preview_without_changing_existing_geometry(self):
        self.add_station(0)
        original = self.window.current_document()["config"]["stations"][0]
        scale = self.window.view.transform().m11()
        self.edit_station(1, width=4480)
        self.edit_station(1, height=1440)
        self.add_station(1)
        self.assertLess(self.window.view.transform().m11(), scale)
        self.assertEqual(self.window.current_document()["config"]["stations"][0], original)
        visible = self.window.view.mapToScene(self.window.view.viewport().rect()).boundingRect()
        for group in self.window.groups.values():
            self.assertTrue(visible.contains(group.sceneBoundingRect()))

    def test_movement_repaints_the_old_and_new_screen_locations(self):
        from PySide6.QtCore import QPointF
        self.window.loaded_peers([])
        doc = copy.deepcopy(self.doc)
        for i, x in enumerate((0, -1600)):
            doc["config"]["stations"][i].update(configured=True, x=x, width=800, height=450)
        self.window.loaded(doc); self.app.processEvents()
        def color_at(x, y):
            point = self.window.view.mapFromScene(QPointF(x, y))
            image = self.window.view.viewport().grab().toImage()
            ratio = image.devicePixelRatio()
            return image.pixelColor(round(point.x() * ratio), round(point.y() * ratio))
        original = color_at(100, 100)
        self.window.groups[0].setX(400)
        self.window.groups[0].setY(100)
        self.app.processEvents()
        self.assertNotEqual(color_at(100, 100), original)
        self.assertEqual(color_at(600, 200), original)
        output = Path(__file__).resolve().parents[2] / "build/desk-layout-numeric-edit.png"
        self.assertTrue(self.window.grab().save(str(output)))

    def test_imported_dimensions_and_window_resize_keep_items_visible(self):
        self.add_station(0)
        self.edit_station(0, x=-1400, y=-800, width=2240, height=720)
        group = self.window.groups[0]
        self.assertEqual((group.pos().x(), group.pos().y()), (-1400, -800))
        self.assertAlmostEqual(group.boundingRect().width(), 2243, delta=3)
        self.assertAlmostEqual(group.boundingRect().height(), 723, delta=3)
        self.window.resize(1500, 950); self.app.processEvents()
        visible = self.window.view.mapToScene(self.window.view.viewport().rect()).boundingRect()
        self.assertTrue(visible.contains(group.sceneBoundingRect()))
        self.window.table.cellWidget(0, 1).click()
        self.assertNotIn(0, self.window.groups)

    def test_dragging_clamps_to_the_configuration_coordinate_range(self):
        self.add_station(0)
        self.window.groups[0].setPos(100500, -100500)
        self.assertEqual(self.window.groups[0].pos().x(), 100000)
        self.assertEqual(self.window.groups[0].pos().y(), -100000)
        self.assertEqual(self.window.current_document()["config"]["stations"][0]["x"], 100000)

    def test_reload_applies_all_values_without_redraw_corrupting_earlier_rows(self):
        for i in range(3):
            self.add_station(i)
        doc = copy.deepcopy(self.doc)
        for i, station in enumerate(doc["config"]["stations"]):
            station.update(configured=i < 3, x=-1234 + i * 2000, y=-800 + i * 300,
                           width=1600 + i * 10, height=900 + i * 10)
        self.window.loaded(doc); self.app.processEvents()
        self.assertEqual(self.window.current_document(), doc)
        self.assertFalse(self.window.dirty)

    def test_export_import_roundtrip_writes_real_file_and_retains_revision(self):
        from PySide6.QtWidgets import QMessageBox
        self.add_station(0)
        self.window.groups[0].setX(-123)
        path = Path(self.directory.name) / "desk"
        self.export_to(path)
        path = path.with_suffix(".json")
        actual = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(actual, self.window.current_document())
        self.assertIn("Exported layout to", self.window.note.text())
        self.window.groups[0].setX(456)
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
            self.import_from(path)
        self.assertEqual(self.window.current_document(), actual)
        self.assertTrue(self.window.dirty)

    def test_offline_draft_exports_then_can_be_imported_against_a_loaded_master(self):
        from PySide6.QtWidgets import QMessageBox
        self.window.loaded(dict(config=default_config(), revision=None))
        self.add_station(1)
        path = Path(self.directory.name) / "offline.json"
        self.export_to(path)
        self.assertIsNone(json.loads(path.read_text())["revision"])
        with patch.object(QMessageBox, "warning") as warning:
            self.window.save()
        warning.assert_called_once()
        self.window.loaded(self.doc)
        self.import_from(path)
        self.window.save(); self.drain()
        self.assertEqual(self.doc["revision"], 11)
        self.assertTrue(self.doc["config"]["stations"][1]["configured"])
        self.assertFalse(self.window.dirty)

    def test_invalid_import_and_io_errors_are_visible_and_leave_edits_intact(self):
        from PySide6.QtWidgets import QMessageBox
        self.add_station(0)
        original = self.window.current_document()
        path = Path(self.directory.name) / "bad.json"
        for value in ([], {"config": default_config()}, dict(config=[], revision=10), "bad JSON"):
            path.write_text(json.dumps(value))
            with patch.object(QMessageBox, "warning") as warning:
                self.import_from(path)
            warning.assert_called_once()
            self.assertEqual(self.window.current_document(), original)
        with patch.object(QMessageBox, "warning") as warning:
            self.export_to(Path(self.directory.name) / "missing" / "desk.json")
        warning.assert_called_once()
        self.assertEqual(self.window.current_document(), original)

    def test_cancelled_file_dialogs_and_discard_prompts_preserve_edits(self):
        from PySide6.QtWidgets import QFileDialog, QMessageBox
        self.add_station(0)
        original = self.window.current_document()
        with patch.object(QFileDialog, "getSaveFileName", return_value=("", "")):
            self.window.export_config()
        with patch.object(QFileDialog, "getOpenFileName", return_value=("", "")):
            self.window.import_config()
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No):
            self.window.reload()
        self.assertEqual(self.window.current_document(), original)
        self.assertFalse(self.window.pending)

    def test_save_persists_every_editor_option_and_rejects_stale_revision(self):
        from PySide6.QtWidgets import QMessageBox
        self.add_station(0)
        self.window.global_enabled.click()
        self.window.delay.setValue(250); self.window.corner.setValue(12)
        self.window.save(); self.drain()
        self.assertFalse(self.doc["config"]["enabled"])
        self.assertEqual(self.doc["config"]["delay_ms"], 250)
        self.assertEqual(self.doc["config"]["corner"], 12)
        self.assertEqual(self.doc["config"]["stations"][0]["width"], 4480)
        self.assertFalse(self.window.dirty)
        self.window.groups[0].setX(100)
        self.doc["revision"] += 1
        with patch.object(QMessageBox, "warning") as warning:
            self.window.save(); self.drain()
        warning.assert_called_once()
        self.assertTrue(self.window.dirty)
        self.assertIn("configuration changed", self.window.note.text())

    def test_edits_made_during_save_survive_its_response(self):
        self.add_station(0)
        commands = []
        with patch.object(self.window, "submit", side_effect=lambda cmd, callback, **args: commands.append((callback, args))):
            self.window.save()
        callback, args = commands[0]
        self.window.groups[0].setX(999)
        callback(dict(config=args["config"], revision=11))
        self.assertEqual(self.window.current_document()["revision"], 11)
        self.assertEqual(self.window.current_document()["config"]["stations"][0]["x"], 999)
        self.assertTrue(self.window.dirty)

    def test_edits_made_during_reload_survive_its_response(self):
        commands = []
        with patch.object(self.window, "submit", side_effect=lambda cmd, callback, **args: commands.append((cmd, callback))):
            self.window.reload()
        self.add_station(0)
        next(callback for cmd, callback in commands if cmd == "config-get")(self.doc)
        self.assertTrue(self.window.table.cellWidget(0, 1).isChecked())
        self.assertTrue(self.window.dirty)

    def test_disconnect_clears_stale_availability_and_reconnect_retries_loading(self):
        self.window.show_status(dict(connected=False, reason="Disconnected"))
        self.assertEqual(self.window.peers[0]["screens"], self.peers[0]["screens"])
        self.assertFalse(self.window.peers[0]["eligible"])
        self.assertEqual(self.window.table.item(0, 3).text(), "Helper absent")
        self.window.loaded(dict(config=default_config(), revision=None))
        self.window.show_status(dict(connected=True, revision=10, topology_revision=1))
        self.drain()
        self.assertEqual(self.window.document["revision"], 10)

    def test_topology_polling_does_not_queue_duplicate_reads(self):
        future = Future(); future.kms_command = "peers"
        self.window.pending.append((future, None))
        with patch.object(self.window, "submit") as submit:
            self.window.show_status(dict(connected=True, revision=10, topology_revision=3))
        submit.assert_not_called()
        self.window.pending.clear()

    def test_snapping_does_not_introduce_an_overlap_with_a_third_station(self):
        from PySide6.QtCore import QPointF, QRectF
        from kms_companion.layout_view import snap_position
        area = QRectF(0, 0, 800, 450)
        neighbors = [QRectF(0, 0, 800, 450), QRectF(800, 30, 800, 450)]
        proposed = QPointF(810, 10)
        self.assertEqual(snap_position(area, proposed, neighbors, 30), proposed)

    def test_offset_screens_snap_into_the_empty_area_and_both_inside_corner_edges(self):
        from PySide6.QtCore import QLineF, QPointF, QRectF
        from kms_companion.layout_view import shared_edges, snap_position
        laptop = QRectF(0, 960, 1920, 1200)
        monitor = QRectF(1920, 0, 3840, 2160)
        screen = QRectF(0, 0, 1366, 768)
        neighbors = [laptop, monitor]
        self.assertEqual(snap_position([screen], QPointF(10, 200), neighbors, 25), QPointF(0, 192))
        corner = snap_position([screen], QPointF(545, 200), neighbors, 25)
        self.assertEqual(corner, QPointF(554, 192))
        self.assertCountEqual(list(shared_edges([screen.translated(corner)], neighbors)),
                              [QLineF(1920, 192, 1920, 960), QLineF(554, 960, 1920, 960)])
        # Moving the multi-screen station uses the same contacts and preserves its offsets.
        self.assertEqual(snap_position(neighbors, QPointF(-10, -8), [screen.translated(corner)], 25), QPointF(0, 0))
        # A third screen in the empty area must still prevent a snap through it.
        blocked = neighbors + [QRectF(600, 200, 200, 200)]
        proposed = QPointF(545, 200)
        self.assertEqual(snap_position([screen], proposed, blocked, 25), proposed)

    def test_contact_lines_exclude_internal_seams_and_point_only_contacts(self):
        from PySide6.QtCore import QLineF, QRectF
        from kms_companion.layout_view import exposed_edges, shared_edges
        screens = [QRectF(0, 100, 100, 100), QRectF(100, 0, 200, 200)]
        edges = list(exposed_edges(screens))
        self.assertIn(("left", 100, 0, 100), edges)
        self.assertNotIn(("left", 100, 0, 200), edges)
        self.assertNotIn(("right", 100, 100, 200), edges)
        self.assertEqual(list(shared_edges([QRectF(0, 0, 100, 100)], screens)),
                         [QLineF(100, 0, 100, 100), QLineF(0, 100, 100, 100)])
        self.assertEqual(list(shared_edges([QRectF(-100, 0, 100, 100)], screens)), [])

    def test_scaled_screen_edges_match_the_controller_integer_coordinates(self):
        self.add_station(0)
        self.edit_station(0, width=3001)
        self.edit_station(0, height=1001)
        _, screens = self.window.station_screens(0)
        self.assertEqual(screens, [dict(x=0, y=0, width=1286, height=750),
                                  dict(x=1286, y=0, width=1715, height=1001)])

    def test_inside_corner_contacts_follow_movement_and_render(self):
        from PySide6.QtCore import QPointF
        self.window.loaded_peers([dict(slot=2, eligible=True, screens=[
            dict(x=0, y=960, width=1920, height=1200), dict(x=1920, y=0, width=3840, height=2160)])])
        doc = copy.deepcopy(self.doc)
        for row, x, y, width, height in ((0, 554, 192, 1366, 768), (1, 0, 0, 5760, 2160)):
            doc["config"]["stations"][row].update(configured=True, x=x, y=y, width=width, height=height)
        self.window.loaded(doc)
        self.window.table.setCurrentCell(0, 0)
        self.app.processEvents()
        self.assertEqual(len(self.window.contact_lines), 2)
        self.assertEqual(self.window.snap_station(0, QPointF(545, 200)), QPointF(554, 192))
        output = Path(__file__).resolve().parents[2] / "build/desk-layout-inside-corner.png"
        self.assertTrue(self.window.grab().save(str(output)))
        self.window.groups[0].setX(500)
        self.assertEqual(len(self.window.contact_lines), 1)
        self.window.groups[0].setY(100)
        self.assertEqual(len(self.window.contact_lines), 0)

    def test_snap_turns_a_small_gap_or_overlap_into_a_shared_edge(self):
        from PySide6.QtCore import QPointF, QRectF
        from kms_companion.layout_view import snap_position
        laptop = QRectF(0, 0, 1920, 1080)
        nuc = QRectF(277, 0, 1366, 768)
        for y in (760, 776):
            with self.subTest(y=y):
                self.assertEqual(snap_position([laptop], QPointF(0, y), [nuc], 8), QPointF(0, 768))
        self.assertEqual(snap_position([laptop], QPointF(0, 777), [nuc], 8), QPointF(0, 777))

    def test_dashed_crossings_show_only_for_the_saved_layout(self):
        from PySide6.QtCore import QLineF, Qt
        doc = copy.deepcopy(self.doc)
        doc["config"]["stations"][1].update(configured=True)
        self.window.loaded(doc)
        line = (277, 1080, 1643, 1080)
        peer = dict(slot=2, eligible=True, screens=[], layout_revision=doc["revision"], remembered_edges=[line])
        self.window.loaded_peers([peer])
        self.window.table.setCurrentCell(1, 0)
        dashed = [item.line() for item in self.window.contact_lines if item.pen().style() == Qt.PenStyle.DashLine]
        self.assertEqual(dashed, [QLineF(*line)])
        self.window.mark_dirty()
        self.assertFalse(any(item.pen().style() == Qt.PenStyle.DashLine for item in self.window.contact_lines))
        self.window.loaded(doc)
        self.window.loaded_peers([dict(peer, layout_revision=doc["revision"] + 1)])
        self.assertFalse(any(item.pen().style() == Qt.PenStyle.DashLine for item in self.window.contact_lines))

    def test_crossing_key_and_canvas_stay_readable_in_both_themes(self):
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QColor, QImage, QPainter, QPalette
        from PySide6.QtWidgets import QLabel
        from kms_companion.layout_view import DESK_BACKGROUND
        original = self.app.palette()
        self.addCleanup(self.app.setPalette, original)
        for color in ("#ffffff", "#151515"):
            with self.subTest(theme=color):
                palette = QPalette(original)
                palette.setColor(QPalette.ColorRole.Window, QColor(color))
                palette.setColor(QPalette.ColorRole.Base, QColor(color))
                self.app.setPalette(palette)
                image = QImage(20, 20, QImage.Format.Format_RGB32)
                painter = QPainter(image)
                self.window.scene.render(painter, QRectF(0, 0, 20, 20), QRectF(-200, -200, 20, 20))
                painter.end()
                self.assertEqual(image.pixelColor(10, 10), QColor(DESK_BACKGROUND))
                self.assertCountEqual([label.text() for label in self.window.crossing_key.findChildren(QLabel)
                                       if label.text()], ["No crossing", "Exact crossing", "Within tolerance",
                                                          "Remembered crossing", "Fallback crossing", "Screen overlap"])

    def test_active_crossings_show_dots_hatching_gap_labels_and_fallback_exit(self):
        from PySide6.QtCore import QLineF, QRectF, Qt
        from PySide6.QtWidgets import QGraphicsSimpleTextItem
        doc = copy.deepcopy(self.doc)
        peers = []
        for slot, x in enumerate((0, 112, 209, 309), 1):
            doc["config"]["stations"][slot - 1].update(x=x, configured=True, width=100, height=100)
            peers.append(dict(self.peers[0], slot=slot, screens=[dict(x=0, y=0, width=100, height=100)],
                              crossings=[], layout_revision=doc["revision"]))
        peers[0]["crossings"] = [dict(line=(100, 0, 100, 100), kind=2, target=2, direction=1)]
        peers[2]["crossings"] = [dict(line=(309, 0, 309, 100), kind=1, target=4, direction=1)]
        peers[3]["crossings"] = [dict(line=(320, 0, 380, 0), kind=4, target=1, direction=2)]
        self.window.loaded(doc)
        self.window.loaded_peers(peers)
        self.window.table.setCurrentCell(0, 0)
        lines = self.window.contact_lines
        self.assertEqual([(item.pen().color().name(), item.pen().style()) for item in lines[:2]],
                         [("#ffffff", Qt.PenStyle.SolidLine), ("#ff5252", Qt.PenStyle.DotLine)])
        self.assertEqual(lines[2].line(), QLineF(309, 0, 309, 100))
        self.assertEqual(lines[2].pen().color().name(), "#ff5252")
        self.assertEqual(lines[3].line(), QLineF(320, 0, 380, 0))
        self.assertEqual(lines[3].pen().style(), Qt.PenStyle.DashDotLine)
        self.assertTrue(any(item.rect() == QRectF(209, 0, 3, 100) and item.brush().style() == Qt.BrushStyle.BDiagPattern
                            for item in self.window.separation_items))
        labels = [item.text() for item in self.window.scene.items() if isinstance(item, QGraphicsSimpleTextItem)]
        self.assertCountEqual(labels, ["12 px gap", "3 px overlap", "\u2191 1"])
        self.window.mark_dirty()
        self.assertEqual(self.window.contact_lines, [])

    def test_save_after_dragging_back_refreshes_joins_but_tolerance_only_save_does_not(self):
        from PySide6.QtCore import QPointF
        self.doc["config"]["stations"][0].update(configured=True, width=4480, height=1440)
        self.window.loaded(self.doc)
        group = self.window.groups[0]
        origin = group.pos()
        group.setPos(origin + QPointF(40, 0))
        group.setPos(origin)
        self.window.save()
        self.drain()
        command = [args for name, args in self.calls if name == "config-set"][-1]
        self.assertTrue(command["refresh_joins"])
        self.assertEqual((command["config"]["stations"][0]["x"], command["config"]["stations"][0]["y"]),
                         (origin.x(), origin.y()))

        self.window.gap_tolerance.setText("4294967295")
        self.window.overlap_tolerance.setText("0")
        self.window.save()
        self.drain()
        command = [args for name, args in self.calls if name == "config-set"][-1]
        self.assertNotIn("refresh_joins", command)
        self.assertEqual(command["config"]["gap_tolerance"], 4294967295)
        self.assertEqual(command["config"]["overlap_tolerance"], 0)

    def test_click_and_drag_screen_inside_another_stations_empty_area(self):
        from PySide6.QtCore import QPointF, Qt
        from PySide6.QtTest import QTest
        self.window.loaded_peers([
            dict(slot=1, eligible=True, screens=[dict(x=0, y=0, width=1366, height=768)]),
            dict(slot=2, eligible=True, screens=[dict(x=0, y=960, width=1920, height=1200),
                                               dict(x=1920, y=0, width=3840, height=2160)])])
        doc = copy.deepcopy(self.doc)
        for row, x, y in ((0, 277, 192), (1, 0, 0)):
            doc["config"]["stations"][row].update(configured=True, x=x, y=y)
        self.window.loaded(doc)
        viewport = self.window.view.viewport()
        for point in (QPointF(500, 400), QPointF(350, 250)):
            with self.subTest(point=point):
                # Dongle 2 starts selected and in front of Dongle 1, as in the report.
                self.window.table.setCurrentCell(1, 0)
                self.app.processEvents()
                start = self.window.view.mapFromScene(point)
                QTest.mouseClick(viewport, Qt.MouseButton.LeftButton, pos=start)
                self.assertTrue(self.window.groups[0].isSelected())
                self.assertFalse(self.window.groups[1].isSelected())
                self.assertEqual(self.window.table.currentRow(), 0)
                self.assertEqual(len(self.window.contact_lines), 1)
                self.window.table.setCurrentCell(1, 0)
                original = self.window.groups[0].pos()
                finish = start + QPointF(-30, -25).toPoint()
                QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
                QTest.mouseMove(viewport, finish, delay=20)
                QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=finish)
                self.app.processEvents()
                self.assertNotEqual(self.window.groups[0].pos(), original)
                self.assertEqual(self.window.groups[1].pos(), QPointF(0, 0))
                self.assertEqual(self.window.table.currentRow(), 0)
                self.window.groups[0].setPos(277, 192)
                self.window.view.fit_layout()

    def test_offline_two_screen_shape_keeps_the_inside_corner_visible_and_selectable(self):
        from PySide6.QtCore import QPointF, Qt
        from PySide6.QtTest import QTest
        peers = [dict(slot=1, eligible=True, ready=True, enabled=True, session=1, age_ms=1,
                      screens=[dict(x=0, y=0, width=1366, height=768)]),
                 dict(slot=2, eligible=False, ready=False, enabled=False, session=0, age_ms=5000,
                      screens=[dict(x=0, y=960, width=1920, height=1200),
                               dict(x=1920, y=0, width=3840, height=2160)])]
        self.window.loaded_peers(peers)
        doc = copy.deepcopy(self.doc)
        doc["config"]["stations"][0].update(configured=True, x=554, y=192, width=1366, height=768)
        doc["config"]["stations"][1].update(configured=True, width=5760, height=2160)
        self.window.loaded(doc)
        self.window.table.setCurrentCell(1, 0)
        self.app.processEvents()
        self.assertEqual(self.window.table.columnCount(), 5)
        self.assertEqual(self.window.table.item(1, 2).text(), "2")
        self.assertEqual(self.window.table.item(1, 3).text(), "Helper absent")
        self.assertEqual(len(self.window.groups[1].screens), 2)
        self.assertFalse(self.window.groups[1].shape().contains(QPointF(1000, 500)))
        QTest.mouseClick(self.window.view.viewport(), Qt.MouseButton.LeftButton,
                         pos=self.window.view.mapFromScene(QPointF(1000, 500)))
        self.assertEqual(self.window.table.currentRow(), 0)
        before = self.window.current_document()
        self.window.show_status(dict(connected=False, reason="Master restarting"))
        self.assertEqual(len(self.window.groups[1].screens), 2)
        self.assertEqual(self.window.current_document(), before)
        peers[1].update(eligible=True, ready=True, enabled=True, session=2, age_ms=1)
        self.window.loaded_peers(peers)
        self.assertEqual(self.window.current_document(), before)
        self.assertEqual(self.window.table.item(1, 3).text(), "Ready")
        output = Path(__file__).resolve().parents[2] / "build/desk-layout-remembered-screens.png"
        self.window.loaded_peers([dict(p, eligible=False, session=0, age_ms=5000) if p["slot"] == 2 else p for p in peers])
        self.assertTrue(self.window.grab().save(str(output)))

    def test_negative_monitor_origins_and_offline_fixed_dimensions(self):
        self.peers[0]["screens"][0].update(x=-1920, y=-200)
        self.peers[0]["screens"][1].update(x=0, y=0)
        self.window.loaded_peers(self.peers)
        self.add_station(0)
        station = self.window.current_document()["config"]["stations"][0]
        self.assertEqual(station["width"], 4480)
        self.assertEqual(station["height"], 1640)
        self.edit_station(1, width=3000)
        self.edit_station(1, height=1500)
        self.add_station(1)
        self.assertAlmostEqual(self.window.groups[1].boundingRect().width(), 3003, delta=3)

    def test_failed_export_does_not_replace_an_existing_file(self):
        from PySide6.QtCore import QSaveFile
        from PySide6.QtWidgets import QMessageBox
        path = Path(self.directory.name) / "existing.json"
        path.write_text("original backup", encoding="utf-8")
        with patch.object(QSaveFile, "commit", return_value=False), patch.object(QMessageBox, "warning") as warning:
            self.export_to(path)
        warning.assert_called_once()
        self.assertEqual(path.read_text(encoding="utf-8"), "original backup")

    def test_import_cancel_and_bom_encoded_backup(self):
        from PySide6.QtWidgets import QMessageBox
        path = Path(self.directory.name) / "backup.json"
        path.write_text(json.dumps(self.doc), encoding="utf-8-sig")
        self.add_station(0)
        original = self.window.current_document()
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No):
            self.import_from(path)
        self.assertEqual(self.window.current_document(), original)
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes):
            self.import_from(path)
        self.assertEqual(self.window.current_document(), self.doc)

    def test_save_response_does_not_rebase_a_stale_import(self):
        self.add_station(0)
        commands = []
        with patch.object(self.window, "submit", side_effect=lambda cmd, callback, **args: commands.append((callback, args))):
            self.window.save()
        callback, args = commands[0]
        imported = copy.deepcopy(self.doc); imported["revision"] = 4
        self.window.loaded(imported); self.window.mark_dirty()
        callback(dict(config=args["config"], revision=11))
        self.assertEqual(self.window.current_document()["revision"], 4)
        self.assertTrue(self.window.dirty)

    def test_local_toggle_is_immediate_and_does_not_dirty_master_layout(self):
        self.window.local_enabled.click(); self.drain()
        self.assertIn(("auto", dict(enabled=False)), self.calls)
        self.assertFalse(self.window.dirty)
        self.window.tray_enabled.trigger(); self.drain()
        self.assertIn(("auto", dict(enabled=False)), self.calls)

    def test_login_startup_reports_failure_and_restores_checkbox(self):
        from PySide6.QtWidgets import QMessageBox
        with patch("kms_companion.cli.startup", side_effect=OSError("Access denied")), patch.object(QMessageBox, "warning") as warning:
            self.window.start_at_login.click()
        warning.assert_called_once()
        self.assertFalse(self.window.start_at_login.isChecked())
        with patch("kms_companion.cli.startup") as startup:
            self.window.start_at_login.click()
        startup.assert_called_once_with(True)
        self.assertFalse(self.window.dirty)

    def test_standalone_setup_launch_about_and_close_routes(self):
        from PySide6.QtGui import QCloseEvent
        from PySide6.QtWidgets import QMessageBox, QSystemTrayIcon
        self.assertIn('Open Firmware Setup', [action.text() for action in self.window.tray_menu.actions()])
        with patch('kms_transport.tools.open_setup') as setup:
            self.window.open_firmware()
            setup.assert_called_once_with(self.window, configure=False)
            setup.reset_mock()
            self.window.restart_dongles()
            setup.assert_called_once_with(self.window, 'restart')
        with patch.object(QMessageBox, "about") as about:
            self.window.about()
        about.assert_called_once()
        with patch.object(QSystemTrayIcon, "isSystemTrayAvailable", return_value=True):
            event = QCloseEvent(); self.window.closeEvent(event)
        self.assertFalse(event.isAccepted())
        self.assertFalse(self.window.isVisible())
        self.window.open_settings()
        self.assertTrue(self.window.isVisible())

    def test_unpaired_mouse_release_cannot_quit_from_tray_menu(self):
        from PySide6.QtCore import QPoint, Qt
        from PySide6.QtTest import QTest
        from PySide6.QtWidgets import QSystemTrayIcon
        menu = self.window.tray_menu
        quit_action = next(action for action in menu.actions() if action.text() == "Quit companion")
        for button in (Qt.MouseButton.RightButton, Qt.MouseButton.LeftButton):
            with self.subTest(button=button):
                QTest.mouseMove(self.window, QPoint(10, 10))
                if os.name == "nt":
                    self.assertIsNone(self.window.tray.contextMenu())
                    self.window.tray.activated.emit(QSystemTrayIcon.ActivationReason.Context)
                    self.assertFalse(menu.isVisible())
                else:
                    menu.popup(QPoint(100, 100))
                self.app.processEvents()
                self.assertTrue(menu.isVisible())
                point = menu.actionGeometry(quit_action).center()
                QTest.mouseMove(menu, point)
                menu.setActiveAction(quit_action)
                try:
                    with patch.object(self.app, "quit") as quit_app:
                        QTest.mouseRelease(menu, button, pos=point)
                        self.app.processEvents()
                        quit_app.assert_not_called()
                    self.assertNotIn(("stop", {}), self.calls)
                    self.assertFalse(self.window.quitting)
                    self.assertTrue(self.window.isVisible())
                finally:
                    menu.hide()
                    self.window.quitting = False

    def test_tray_menu_keeps_ownership_and_supports_left_click_and_keyboard(self):
        import gc
        from PySide6.QtCore import QPoint, Qt
        from PySide6.QtTest import QTest
        menu = self.window.tray_menu
        gc.collect()
        self.assertIs(menu.parent(), self.window)
        if os.name != "nt":
            self.assertIs(menu, self.window.tray.contextMenu())
        quit_action = next(action for action in menu.actions() if action.text() == "Quit companion")
        try:
            for selection in ("left click", "keyboard"):
                with self.subTest(selection=selection), patch.object(self.app, "quit") as quit_app, \
                     self.assertLogs("kms_companion.ui", level="INFO") as logs:
                    QTest.mouseMove(self.window, QPoint(10, 10))
                    menu.popup(QPoint(100, 100)); self.app.processEvents()
                    menu.setActiveAction(quit_action)
                    if selection == "left click":
                        point = menu.actionGeometry(quit_action).center()
                        QTest.mouseMove(menu, point)
                        self.app.processEvents()
                        QTest.mouseClick(menu, Qt.MouseButton.LeftButton, pos=point)
                    else:
                        QTest.keyClick(menu, Qt.Key.Key_Return)
                    self.app.processEvents()
                    quit_app.assert_called_once()
                    self.assertIn("Quit companion requested from tray menu", "\n".join(logs.output))
                    self.assertIn(("stop", {}), self.calls)
                    self.assertTrue(self.window.quitting)
                    self.window.quitting = False
        finally:
            menu.hide()

    def test_device_and_clipboard_settings_tabs_submit_their_values(self):
        from PySide6.QtWidgets import QDialog, QFormLayout, QPushButton, QComboBox, QLineEdit
        values = dict(trigger=1, normal_led=False, brightness=10, keyboard_leds=True, boot_slot=2,
                      jiggler_seconds=30, double_ms=350, select_ms=3000,
                      slot_colors=[1, 2, 3, 4, 5, 6, 7], jiggler_distance=1)
        self.window.edit_preferences(dict(revision=5, values=values))
        dialog = self.window.device_form
        fields = {}
        form = dialog.layout()
        for row in range(form.rowCount()):
            label = form.itemAt(row, QFormLayout.ItemRole.LabelRole)
            if label:
                fields[label.widget().text()] = form.itemAt(row, QFormLayout.ItemRole.FieldRole).widget()
        fields["Switching trigger"].setCurrentIndex(2)
        fields["Controller LED brightness"].setValue(99)
        colour = fields["Dongle 7 colour"]
        self.assertEqual(colour.currentText(), 'White')
        self.assertFalse(colour.itemIcon(2).isNull())
        colour.setCurrentIndex(colour.findText('Green'))
        self.assertEqual(fields['Startup dongle'].currentText(), 'Dongle 2')
        fields['Startup dongle'].setCurrentIndex(0)
        dialog.findChild(QPushButton).click(); self.drain()
        command = next(args for cmd, args in self.calls if cmd == "preferences-set")
        self.assertEqual(command["revision"], 5)
        self.assertEqual(command["values"]["trigger"], 3)
        self.assertEqual(command["values"]["brightness"], 99)
        self.assertEqual(command['values']['boot_slot'], 0)
        self.assertEqual(command["values"]["slot_colors"][-1], 2)
        self.window.clipboard_preferences()
        panel = self.window.clipboard_panel
        panel.name.setText("Desk laptop")
        panel.mode.setCurrentIndex(0)
        panel.save.click(); self.drain()
        settings = next(args["settings"] for cmd, args in self.calls if cmd == "clipboard-settings")
        self.assertEqual(settings["name"], "Desk laptop")
        self.assertEqual(settings["max_mb"], 256)

    def test_quit_keeps_unsaved_work_unless_discard_is_confirmed(self):
        from PySide6.QtWidgets import QMessageBox
        self.add_station(0)
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No), patch.object(self.app, "quit") as quit_app:
            self.window.quit_companion()
        quit_app.assert_not_called()
        self.assertNotIn(("stop", {}), self.calls)
        with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.Yes), patch.object(self.app, "quit") as quit_app:
            self.window.quit_companion()
        quit_app.assert_called_once()
        self.assertIn(("stop", {}), self.calls)
