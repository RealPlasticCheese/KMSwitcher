"""Qt integration checks; skip cleanly for CLI-only installations."""
import copy
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from kms_companion.protocol import default_config

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "Optional UI dependencies not installed")
class UiTests(unittest.TestCase):
    def test_shortcuts_show_saved_trigger_and_remain_available_offline(self):
        from PySide6.QtWidgets import QApplication, QTextBrowser
        from kms_companion.ui import launch
        app = QApplication.instance() or QApplication([])

        def call(command, **args):
            if command == "preferences-get":
                return {"values": dict(trigger=5, double_ms=450, select_ms=2500)}
            raise RuntimeError("Offline test")

        with patch("kms_companion.ipc.call", side_effect=call) as helper, \
             patch("kms_companion.cli.startup_enabled", return_value=False):
            window = launch(execute=False)
            window.timer.stop()

            def process_results():
                window.pool.submit(lambda: None).result(timeout=3)
                window.status_ticks = 0
                window.tick()

            try:
                panel = window.shortcuts_panel
                window.select_settings_tab("Shortcuts")
                process_results()
                self.assertIs(window.tabs.currentWidget(), panel)
                for setting in ("Insert", "450 ms", "2,500 ms"):
                    self.assertIn(setting, panel.trigger.text())

                helper.side_effect = RuntimeError("Disconnected")
                window.select_settings_tab("General")
                window.select_settings_tab("Shortcuts")
                process_results()
                self.assertIn("Ctrl by default", panel.trigger.text())
                guide = panel.findChild(QTextBrowser).toPlainText()
                for shortcut in ("Switch dongle", "Add a dongle", "startup dongle",
                                 "Toggle the mouse jiggler", "Cycle the mouse jiggler mode",
                                 "Change the trigger key", "Cancel a shortcut",
                                 "Type diagnostic logs", "Open Keyboard Console"):
                    self.assertIn(shortcut, guide)
            finally:
                window.pool.shutdown(wait=True)
                window.transfer_pool.shutdown(wait=True)
                window.device_pool.shutdown(wait=True)
                window.tray.hide()
                window.hide()

    def test_callback_exception_is_logged_and_remaining_results_are_processed(self):
        from concurrent.futures import Future
        from PySide6.QtWidgets import QApplication
        from kms_companion.ui import launch
        app = QApplication.instance() or QApplication([])
        with patch('kms_companion.ipc.call', side_effect=RuntimeError('Offline test')), \
             patch('kms_companion.cli.startup_enabled', return_value=False):
            window = launch(execute=False)
            window.timer.stop()
            try:
                window.pool.shutdown(wait=True)
                window.pending.clear()
                failed, good = Future(), Future()
                failed.kms_command = good.kms_command = 'status'
                failed.set_result({})
                good.set_result('still running')
                received = []
                def broken_callback(result):
                    raise TypeError('callback failure marker')
                window.pending = [(failed, broken_callback), (good, received.append)]
                with self.assertLogs('kms_companion.ui', level='ERROR') as logs:
                    window.tick()
                self.assertEqual(received, ['still running'])
                self.assertFalse(window.quitting)
                self.assertIn('Traceback', '\n'.join(logs.output))
                self.assertIn('callback failure marker', window.note.text())
            finally:
                window.transfer_pool.shutdown(wait=True)
                window.tray.hide()
                window.hide()

    def test_companion_polling_survives_in_a_separate_interpreter(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", KMS_COMPANION_SMOKE="1",
                               LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory)
            result = subprocess.run([sys.executable, "-c", "from kms_companion.gui import main; raise SystemExit(main())"],
                                    env=environment, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: companion UI polling runtime", result.stdout)

    def test_helper_outage_polling_is_quiet_and_recovers(self):
        from PySide6.QtWidgets import QApplication, QSystemTrayIcon
        from kms_companion.ui import launch
        app = QApplication.instance() or QApplication([])
        error = "Background helper is not running; start 'kmswitcher run' or 'kmswitcher ui'"

        def drain(window):
            end = time.monotonic() + 3
            while window.pending and time.monotonic() < end:
                window.status_ticks = 0
                window.tick()
                app.processEvents()
                time.sleep(.01)
            self.assertFalse(window.pending)

        with patch("kms_companion.ipc.call", side_effect=RuntimeError(error)) as call, \
             patch("kms_companion.cli.startup_enabled", return_value=False), \
             patch.object(QSystemTrayIcon, "showMessage") as notify:
            window = launch(execute=False)
            window.timer.stop()
            try:
                drain(window)
                for _ in range(10):
                    # Exercise the regular 200 ms clipboard polling path during an outage.
                    window.status_ticks = 1
                    window.tick()
                    drain(window)
                self.assertGreaterEqual(sum(c.args[0] == "clipboard-status" for c in call.call_args_list), 11)
                self.assertEqual(window.note.text(), error)
                notify.assert_not_called()

                # An explicit transfer still reports its failure once.
                window.submit("clipboard-send", slot=2, capture="unused.json")
                drain(window)
                notify.assert_called_once_with("Clipboard", error)
                notify.reset_mock()

                # Polling continues and reconnects without restarting the tray app.
                state = dict(settings=dict(mode="network-preferred", name="Laptop", max_mb=256),
                    items=[], peers=[dict(slot=2, available=True, reason="Ready")])
                call.side_effect = None
                call.return_value = state
                window.status_ticks = 1
                window.tick()
                drain(window)
                self.assertEqual(window.clipboard_state, state)
                self.assertTrue(window.send_actions[2].isEnabled())
                notify.assert_not_called()
            finally:
                window.timer.stop()
                window.pool.shutdown(wait=True)
                window.transfer_pool.shutdown(wait=True)
                window.tray.hide()
                window.hide()

    def test_station_layout_and_local_toggle_share_helper_commands(self):
        from PySide6.QtWidgets import QApplication
        from PySide6.QtGui import QFontDatabase, QFont
        from kms_companion.ui import launch
        app = QApplication.instance() or QApplication([])
        if os.name == "nt":
            QFontDatabase.addApplicationFont("C:/Windows/Fonts/segoeui.ttf")
            app.setFont(QFont("Segoe UI", 10))
        config = default_config()
        for slot, x, y in ((1, 0, -768), (2, 0, 0), (3, 4600, 0)):
            config["stations"][slot - 1].update(configured=True, x=x, y=y)
        calls = []
        def call(command, **args):
            calls.append((command, args))
            if command == "status":
                return dict(slot=2, selected=2, enabled=True, backend="Windows", revision=10, reason="Ready")
            if command == "clipboard-status":
                return dict(settings=dict(mode="network-preferred", name="Laptop", max_mb=256), items=[],
                    peers=[dict(slot=i, name=f"Computer {i}", available=True, reason="Ready") for i in (1, 2, 3, 4)])
            if command == "config-get":
                return {"config": copy.deepcopy(config), "revision": 10}
            if command == "peers":
                peers = [dict(slot=i, eligible=True, ready=True, enabled=True, session=i, age_ms=1,
                    screens=[dict(x=0, y=0, width=1366 if i == 1 else 1920, height=768 if i == 1 else 1080)]) for i in (1, 2, 3)]
                peers[1]["screens"].append(dict(x=1920, y=0, width=2560, height=1440))
                return peers
            return args
        with patch("kms_companion.ipc.call", side_effect=call), \
             patch("kms_companion.cli.startup_enabled", return_value=True), \
             patch("kms_companion.cli.startup") as startup:
            window = launch(execute=False)
            self.assertTrue(window.start_at_login.isChecked())
            startup.assert_not_called()
            window.start_at_login.setChecked(False)
            startup.assert_called_once_with(False)
            end = time.monotonic() + 3
            while window.pending and time.monotonic() < end:
                app.processEvents(); time.sleep(.01)
            self.assertIsNotNone(window.document)
            peers = list(window.peers)
            window.loaded_peers([dict(slot=2, eligible=False, age_ms=5000, session=1, enabled=True, ready=False,
                power_supported=True, power_state="sleep_or_hibernate", power_fresh=False)])
            self.assertEqual(window.table.item(1, 4).text(), "Last reported: Sleep / hibernate requested")
            window.loaded_peers(peers)
            self.assertEqual(set(window.groups), {0, 1, 2})
            root_actions = window.tray_menu.actions()
            self.assertNotIn("Local dongle logs", [action.text() for action in root_actions])
            self.assertTrue(window.transfer_send_button.isEnabled())
            for slot in (1, 3, 4):
                self.assertIn(window.send_actions[slot], root_actions)
                self.assertIn(window.send_actions[slot], window.transfer_send_menu.actions())
                self.assertIsNone(window.send_actions[slot].menu())
                self.assertTrue(window.send_actions[slot].isEnabled())
            self.assertFalse(window.send_actions[2].isVisible())
            with patch("kms_companion.clipboard_ui.capture_clipboard", return_value="capture.json"), \
                 patch.object(window, "submit") as submit:
                window.transfer_send_menu.actions()[0].trigger()
                submit.assert_called_once_with("clipboard-send", slot=1, capture="capture.json")
            with patch("kms_companion.clipboard_ui.publish_clipboard") as publish:
                state = dict(settings=dict(mode="network-preferred", name="Laptop", max_mb=256), peers=[],
                    items=[dict(id=51, direction="receive", slot=1, state="Ready", kind="content")])
                window.show_clipboard_status(state)
                self.assertFalse(window.send_actions[1].isEnabled())
                self.assertFalse(window.send_actions[4].isVisible())
                self.assertFalse(any(c == "clipboard-content" for c, _ in calls))
                state["items"].append(dict(id=52, direction="receive", slot=3, state="Ready", kind="text"))
                window.show_clipboard_status(state)
                end = time.monotonic() + 3
                while window.pending and time.monotonic() < end:
                    app.processEvents(); time.sleep(.01)
                publish.assert_called_once()
                self.assertIn(("clipboard-content", dict(id=52)), calls)
            state["items"].append(dict(id=53, direction="receive", slot=1, state="Saved",
                                       kind="files", saved_path="saved.zip", prepared=True))
            window.timer.stop(); window.clipboard_state = state
            window.show_transfers(); app.processEvents()
            from PySide6.QtGui import QDesktopServices
            from PySide6.QtCore import Qt
            from PySide6.QtWidgets import QLabel, QMessageBox, QPushButton
            table = window.transfer_table
            self.assertEqual([table.item(row, 0).data(Qt.ItemDataRole.UserRole) for row in range(3)], [53, 52, 51])
            self.assertEqual(table.horizontalHeaderItem(0).text(), "ID")
            self.assertEqual(table.item(0, 0).text(), "00000035")
            actions = table.cellWidget(0, 6)
            self.assertEqual(table.horizontalScrollBar().maximum(), 0)
            self.assertEqual(len(actions.findChildren(QPushButton)), 6)
            self.assertGreaterEqual(actions.width(), actions.minimumSizeHint().width())
            self.assertGreaterEqual(actions.height(), actions.minimumSizeHint().height())
            with patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No) as confirm, \
                 patch.object(window, "submit") as submit:
                window.transfer_clear_button.click()
                self.assertIn("delete all clipboard items in the clipboard folder", confirm.call_args.args[2])
                self.assertEqual(confirm.call_args.args[-1], QMessageBox.StandardButton.No)
                submit.assert_not_called()
                confirm.return_value = QMessageBox.StandardButton.Yes
                window.transfer_clear_button.click()
                self.assertEqual(submit.call_args.args, ("clipboard-clear",))
                submit.call_args.kwargs["callback"]({})
                self.assertTrue(window.transfer_clear_button.isEnabled())
                self.assertEqual(submit.call_args.args, ("clipboard-status",))
                window.transfer_clear_by_type["image"].click()
                self.assertIn("delete clipboard items of type Images", confirm.call_args.args[2])
                self.assertEqual(confirm.call_args.args[-1], QMessageBox.StandardButton.No)
                self.assertEqual(submit.call_args.args, ("clipboard-clear",))
                self.assertEqual(submit.call_args.kwargs["kind"], "image")
                submit.call_args.kwargs["callback"]({})

            dongles = window.device_management
            initial_width = dongles.slot.sizeHint().width()
            dongles.show_dongles(dict(paired=[1], selected=1, pairing=False))
            self.assertGreater(dongles.slot.sizeHint().width(), initial_width)
            folder_link = next(label for label in window.transfer_dialog.findChildren(QLabel)
                               if "Open transfer folder" in label.text())
            with tempfile.TemporaryDirectory() as profile, \
                 patch("kms_companion.agent.config_directory", return_value=Path(profile)), \
                 patch.object(QDesktopServices, "openUrl", return_value=True) as open_folder:
                folder_link.linkActivated.emit("clipboard-folder")
                self.assertEqual(Path(open_folder.call_args.args[0].toLocalFile()), Path(profile) / "clipboard")
                self.assertTrue((Path(profile) / "clipboard").is_dir())
            preview = Path(__file__).resolve().parents[2] / "build/clipboard-transfers-ui.png"
            preview.parent.mkdir(parents=True, exist_ok=True)
            self.assertTrue(window.transfer_dialog.grab().save(str(preview)))
            window.transfer_dialog.hide()
            window.groups[0].setPos(100, -800)
            app.processEvents()
            self.assertEqual(window.current_document()["config"]["stations"][0]["x"], 100)
            self.assertEqual(window.current_document()["config"]["stations"][0]["y"], -800)
            self.assertTrue(window.dirty)
            self.assertEqual(window.current_document()["revision"], 10)
            output = Path(__file__).resolve().parents[2] / "build/companion-ui.png"
            output.parent.mkdir(parents=True, exist_ok=True)
            self.assertTrue(window.grab().save(str(output)))
            window.timer.stop(); window.pool.shutdown(wait=True); window.transfer_pool.shutdown(wait=True); window.tray.hide(); window.hide()
