"""The Companion maintenance entry point can restart without a firmware bundle."""
from argparse import Namespace
import importlib.util
import os
import unittest
from unittest.mock import Mock, patch

from kms_update import __main__ as cli, firmware, management, releases


class RestartCommandTests(unittest.TestCase):
    def test_restart_needs_no_release_files_and_uses_explicit_targets(self):
        for all_slots in (False, True):
            args = Namespace(command='restart', port='fake', all=all_slots, slot=[2], product='regular', bundle=None)
            controller = Mock(); controller.peers.return_value = ([1, 2, 3], 2)
            with patch.object(cli, 'open_controller'), patch.object(management, 'Controller', return_value=controller), \
                 patch.object(management, 'restart_targets', return_value={'success': True}) as restart, \
                 patch.object(firmware, 'Bundle') as bundle:
                self.assertTrue(cli.execute(args)['success'])
                restart.assert_called_once_with(controller, [1, 2, 3] if all_slots else [2])
                bundle.assert_not_called()


@unittest.skipUnless(importlib.util.find_spec('PySide6'), 'Optional Qt dependencies not installed')
class RestartUiTests(unittest.TestCase):
    def test_companion_restart_page_selects_one_or_all_and_invalidates_changed_port(self):
        os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
        from PySide6.QtWidgets import QApplication, QMessageBox
        from kms_update.ui import launch
        app = QApplication.instance() or QApplication([])
        with patch.object(firmware, 'ports', return_value=[dict(port='COM-test', description='S3')]), \
             patch.object(releases, 'default_source', return_value=None):
            window = launch(execute=False, initial_task='restart')
            try:
                self.assertEqual(window.tabs.tabText(window.tabs.currentIndex()), 'Restart dongles')
                window.restart_port.setCurrentIndex(1)
                window.restart_inspected(dict(devices=[dict(slot=0, identity=dict(capabilities=16)),
                    dict(slot=2, identity=dict(capabilities=16)), dict(slot=3, error='offline')]))
                self.assertEqual(window.restart_target.count(), 4)
                for target, arguments in ((2, ['--slot', '2']), ('all', ['--all'])):
                    window.restart_target.setCurrentIndex(window.restart_target.findData(target))
                    with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes), \
                         patch.object(window, 'command') as command:
                        window.restart_devices()
                        command.assert_called_once_with('restart', arguments, port='COM-test')
                with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.No), \
                     patch.object(window, 'command') as command:
                    window.restart_devices(); command.assert_not_called()
                window.show(); app.processEvents()
                window.restart_port.setCurrentIndex(0)
                self.assertIsNone(window.restart_target.currentData())
                # A valid port on the firmware page must not substitute for an
                # unselected port on the restart page.
                window.port.setCurrentIndex(1)
                with patch.object(window, 'start_process') as start, patch.object(window, 'failure') as failure:
                    window.command('inspect', [], port=None)
                    start.assert_not_called(); failure.assert_called_once_with('Select a serial port')
            finally:
                window.timer.stop(); window.close()
