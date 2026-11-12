"""Stored-S3 receiver updates must never load or upload a local replacement image."""
from argparse import Namespace
from contextlib import ExitStack
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_update import __main__ as cli, firmware, management, releases

STORED = dict(git='stored-revision', sha256='a' * 64, size=803808, chip=2, capabilities=3)


class StoredReceiverCommandTests(unittest.TestCase):
    def test_cached_update_needs_no_bundle_and_never_stages_an_image(self):
        for all_targets in (False, True):
            with self.subTest(all_targets=all_targets), ExitStack() as stack:
                bundle = stack.enter_context(patch.object(firmware, 'Bundle', side_effect=AssertionError('No local firmware needed')))
                stack.enter_context(patch.object(cli, 'open_controller'))
                controller = stack.enter_context(patch.object(management, 'Controller')).return_value
                controller.peers.return_value = ([1, 3], 3)
                stack.enter_context(patch.object(management, 'cache_identity', return_value=STORED))
                stage = stack.enter_context(patch.object(management, 'stage_cache'))
                update = stack.enter_context(patch.object(management, 'update_targets', return_value=[dict(slot=3, result='updated')]))
                master = stack.enter_context(patch.object(management, 'update_master'))
                args = Namespace(command='ota', bundle=Path('missing'), product='regular', role='receiver',
                    port='fake', slot=[3], all=all_targets, force=True, cached=True, cached_sha256=STORED['sha256'])
                self.assertTrue(cli.execute(args)['success'])
                update.assert_called_once_with(controller, [1, 3] if all_targets else [3], 3, b'', STORED, True, cached=True)
                bundle.assert_not_called(); stage.assert_not_called(); master.assert_not_called()

    def test_empty_changed_or_unsupported_cache_cannot_start_updates(self):
        for cached, error in ((None, None), (dict(STORED, sha256='b' * 64), None),
                              (None, management.ManagementError(management.UNSUPPORTED))):
            with self.subTest(cached=cached, error=error), patch.object(cli, 'open_controller'), \
                 patch.object(management, 'Controller') as factory, \
                 patch.object(management, 'cache_identity', return_value=cached, side_effect=error), \
                 patch.object(management, 'update_targets') as update:
                factory.return_value.peers.return_value = ([3], 3)
                args = Namespace(command='ota', bundle=Path('missing'), product='regular', role='receiver',
                    port='fake', slot=[3], all=False, force=False, cached=True, cached_sha256=STORED['sha256'])
                with self.assertRaises((ValueError, management.ManagementError)): cli.execute(args)
                update.assert_not_called()

    def test_cached_controller_update_is_rejected_before_opening_port(self):
        args = Namespace(command='ota', cached=True, role='controller')
        with patch.object(cli, 'open_controller') as open_port:
            with self.assertRaisesRegex(ValueError, 'receivers only'): cli.execute(args)
            open_port.assert_not_called()

    def test_inspection_reports_stored_image_without_requiring_a_release(self):
        with patch.object(firmware, 'Bundle', side_effect=AssertionError('No bundle for inspection')), \
             patch.object(cli, 'open_controller'), patch.object(management, 'Controller') as factory, \
             patch.object(management, 'cache_identity', return_value=STORED):
            factory.return_value.peers.return_value = ([3], 3)
            factory.return_value.identity.return_value = dict(chip=2, git='running', capabilities=0)
            result = cli.execute(Namespace(command='inspect', port='fake', bundle=Path('missing'), product='regular'))
            self.assertEqual(result['receiver_cache'], STORED)
            self.assertEqual([device['slot'] for device in result['devices']], [0, 3])


class StoredReceiverWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from kms_update.ui import launch
        self.stack = ExitStack(); self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(firmware, 'ports', return_value=[]))
        self.stack.enter_context(patch.object(releases, 'default_source', return_value=None))
        folder = self.stack.enter_context(tempfile.TemporaryDirectory())
        self.window = launch(bundle=Path(folder) / 'missing', execute=False)
        self.addCleanup(self.window.close)
        self.window.port.addItem('Test controller', 'COM8'); self.window.port.setCurrentIndex(1)
        self.window.method.setCurrentIndex(self.window.method.findData('ota'))
        identity = dict(git='running', sha256='0' * 64, capabilities=15)
        self.window.inspected(dict(devices=[dict(slot=slot, identity=identity) for slot in (0, 1, 3)],
                                   receiver_cache=STORED))

    def choose_stored(self):
        self.window.receiver_source.setCurrentIndex(self.window.receiver_source.findData('cache'))

    def test_source_selection_shows_identity_preserves_dongles_and_excludes_controller(self):
        from PySide6.QtCore import Qt
        for row in (0, 2): self.window.list.item(row).setCheckState(Qt.CheckState.Checked)
        self.choose_stored()
        self.assertEqual(self.window.list.item(0).checkState(), Qt.CheckState.Unchecked)
        self.assertFalse(self.window.list.item(0).flags() & Qt.ItemFlag.ItemIsEnabled)
        self.assertEqual(self.window.list.item(2).checkState(), Qt.CheckState.Checked)
        self.assertIn(STORED['sha256'], self.window.cache_label.text())
        self.assertIn('Local release files are not used', self.window.bundle_label.text())
        self.window.receiver_source.setCurrentIndex(0)
        self.assertTrue(self.window.list.item(0).flags() & Qt.ItemFlag.ItemIsEnabled)
        self.assertNotIn('already stored', self.window.instructions.text())

    def test_review_uses_stored_image_hash_and_only_checked_dongles_without_local_firmware(self):
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QMessageBox
        self.choose_stored(); self.window.list.item(2).setCheckState(Qt.CheckState.Checked)
        with patch.object(self.window, 'command') as command, patch.object(self.window, 'failure') as failure, \
             patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes) as review:
            self.window.update_devices()
            command.assert_called_once_with('ota', ['--role', 'receiver', '--slot', '3', '--speed', 'conservative', '--cached', '--cached-sha256', STORED['sha256']])
            failure.assert_not_called()
            self.assertIn(STORED['sha256'], review.call_args.args[2])
            self.assertIn('No receiver image will be uploaded', review.call_args.args[2])

    def test_changing_port_invalidates_cached_identity_and_device_selection(self):
        self.choose_stored()
        self.window.port.addItem('Other controller', 'COM9'); self.window.port.setCurrentIndex(2)
        self.assertIsNone(self.window.stored_receiver)
        self.assertFalse(self.window.devices)
        self.assertIn('Inspect the controller', self.window.cache_label.text())

    def test_empty_cache_cannot_be_reviewed_or_started(self):
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QMessageBox
        self.choose_stored()
        self.window.inspected(dict(devices=[dict(slot=3, error='unreachable')], receiver_cache=None))
        self.window.list.item(0).setCheckState(Qt.CheckState.Checked)
        with patch.object(self.window, 'command') as command, patch.object(self.window, 'failure') as failure, \
             patch.object(QMessageBox, 'question') as review:
            self.window.update_devices()
            command.assert_not_called(); review.assert_not_called(); failure.assert_called_once()
