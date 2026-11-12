import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from kms_companion.protocol import default_config

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')


@unittest.skipUnless(importlib.util.find_spec('PySide6'), 'Optional UI dependencies not installed')
class DiagnosticsUiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_local_event_filter_export_and_incomplete_capture(self):
        from kms_companion.diagnostics_ui import create_dialog
        dialog = create_dialog(None, lambda: None)
        self.addCleanup(dialog.close)
        sample = dict(loading=False, supported=True, error='', count=2, boot='00000016', overwritten=12, entries=[
            dict(boot='0000000b', first_ms=120, last_ms=150, repeats=3, description='Radio receive gap', detail=1000),
            dict(boot='00000016', first_ms=50, last_ms=50, repeats=1, description='USB mounted', detail=0)])
        dialog.show_state(sample)
        self.assertEqual(dialog.table.rowCount(), 2); self.assertIn('12 overwritten', dialog.summary.text())
        dialog.filter.setText('receive'); self.assertEqual(dialog.table.rowCount(), 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'logs.json'
            with patch('PySide6.QtWidgets.QFileDialog.getSaveFileName', return_value=(str(path), '')):
                dialog.export_logs()
            self.assertEqual(json.loads(path.read_text())['entries'], sample['entries'])
        dialog.show_state(dict(sample, loading=True)); self.assertFalse(dialog.export.isEnabled())
        dialog.show_state(dict(sample, error='USB disconnected')); self.assertFalse(dialog.export.isEnabled())
        self.assertIn('Incomplete', dialog.summary.text())
        dialog.show_state(dict(sample, entries=[])); self.assertEqual(dialog.table.rowCount(), 0)

    def test_recovery_popups_are_consumed_once_and_can_be_disabled(self):
        from kms_companion.ui import launch
        from PySide6.QtWidgets import QSystemTrayIcon
        def call(command, **args):
            if command == 'status': return dict(connected=False, enabled=True, recovery_session=123, recovery_events=[])
            if command == 'clipboard-status': return dict(settings={}, items=[], peers=[])
            if command == 'config-get': return dict(config=default_config(), revision=None)
            if command == 'peers': return []
            return args
        with patch('kms_companion.ipc.call', side_effect=call) as ipc_call, patch('kms_companion.cli.startup_enabled', return_value=False), \
             patch.object(QSystemTrayIcon, 'showMessage') as popup:
            window = launch(execute=False); window.timer.stop()
            try:
                until = time.monotonic() + 3
                while window.pending and time.monotonic() < until:
                    window.status_ticks = 0; window.tick(); self.app.processEvents(); time.sleep(.01)
                ipc_call.assert_any_call('peers', include_joins=True)
                base = dict(connected=False, enabled=True, recovery_session=123, recovery_notifications=True)
                event = dict(id=1, notify=True, message='Dongle 2 restarted. Ready again.')
                window.show_status(dict(base, recovery_events=[event])); window.show_status(dict(base, recovery_events=[event]))
                popup.assert_called_once_with('KMSwitcher recovery', event['message'])
                window.show_status(dict(base, recovery_notifications=False, recovery_events=[dict(event, id=2)]))
                self.assertEqual(popup.call_count, 1)
                window.show_status(dict(base, recovery_events=[dict(event, id=2)])); self.assertEqual(popup.call_count, 1)
                window.recovery_notifications.click()
                for future, _ in window.pending: future.result(timeout=3)
                ipc_call.assert_any_call('recovery-notifications', enabled=False)
            finally:
                window.pool.shutdown(wait=True); window.transfer_pool.shutdown(wait=True)
                window.tray.hide(); window.hide()


if __name__ == '__main__': unittest.main()
