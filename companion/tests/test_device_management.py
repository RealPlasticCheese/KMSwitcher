import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import zlib

from kms_companion.device_control import execute
from kms_companion.protocol import Message, Op, words
from kms_companion.remote_diagnostics import RemoteDiagnostics, usb_inventory
from kms_companion.transport import RejectedCommand
from kms_transport import tools


class DeviceControlTests(unittest.TestCase):
    def test_jiggler_mode_is_addressed_without_selecting_the_dongle(self):
        receiver = Mock()
        receiver.request.return_value = Message(Op.DEVICE_CONTROL, data=words(12, 4, 5, 3, 2, 1, 1, 1))
        result = execute(receiver, 'device-jiggler-mode', dict(slot=3, value=2))
        receiver.request.assert_called_once_with(Op.DEVICE_CONTROL, words(7, 3, 2), timeout=.6)
        self.assertTrue(result['enabled'])
        self.assertTrue(result['pending'])
        self.assertEqual(result['mode'], 2)

    def test_old_firmware_guidance_and_no_retry_of_a_mutation(self):
        receiver = Mock()
        receiver.request.side_effect = RejectedCommand('unsupported firmware')
        with self.assertRaisesRegex(RuntimeError, 'Update the controller and attached dongle'):
            execute(receiver, 'device-unpair', dict(slot=3))
        self.assertEqual(receiver.request.call_count, 1)

    def test_usb_snapshot_reads_each_record_on_the_same_token(self):
        def call(name, **args):
            if name == 'device-usb-snapshot': return dict(token=456, devices=1, interfaces=1)
            self.assertEqual(args, dict(token=456, index=0))
            return dict(address=7, device_class=3)
        self.assertEqual(usb_inventory(call)['interfaces'][0]['address'], 7)

    def test_diagnostics_relay_cannot_send_ota_commands(self):
        receiver = Mock()
        with self.assertRaises(ValueError):
            execute(receiver, 'device-remote-start', dict(slot=2, operation=6))
        receiver.request.assert_not_called()


class RemoteHistoryTests(unittest.TestCase):
    def client(self, history, *, checksum=None, change_boot=False):
        state = dict(data=b'', token=0, released=False)
        def call(command, **args):
            if command == 'device-remote-start':
                self.assertEqual(args['slot'], 3)
                op, offset = args['operation'], args['offset']
                if op == 1: state['data'] = b'identity'
                elif op == 3:
                    state['data'] = words(987, len(history), zlib.crc32(history) if checksum is None else checksum)
                elif op == 4:
                    self.assertEqual(args['data'], words(987).hex())
                    self.assertEqual(args['boot'], 234)
                    state['data'] = history[offset:offset + 192]
                    if offset == len(history): state['released'] = True
                state['token'] += 1
                return dict(token=state['token'])
            self.assertEqual(args['token'], state['token'])
            offset = args['offset']
            return dict(pending=False, status=0, length=len(state['data']),
                        boot=235 if change_boot and offset else 234, data=state['data'][offset:offset + 32].hex())
        return RemoteDiagnostics(call, 3), state

    def test_history_assembles_pages_and_releases_snapshot(self):
        expected = b'KMSwitcher retained diagnostic history\n' * 10
        client, state = self.client(expected)
        self.assertEqual(client.history(), expected)
        self.assertTrue(state['released'])

    def test_corrupt_history_is_rejected_and_snapshot_released(self):
        client, state = self.client(b'retained events', checksum=0)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            client.history()
        self.assertTrue(state['released'])

    def test_boot_change_between_pages_is_rejected(self):
        client, _ = self.client(b'diagnostics' * 20, change_boot=True)
        with self.assertRaisesRegex(ValueError, 'changed'):
            client.history()


class ToolLauncherTests(unittest.TestCase):
    def test_saved_portable_setup_and_stale_path_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / ('kms-setup.exe' if os.name == 'nt' else 'kms-setup')
            executable.write_text('launcher'); executable.chmod(0o755)
            missing = Path(folder) / 'missing'
            with patch.object(tools, 'settings_path', return_value=Path(folder) / 'tools.json'):
                tools.save_settings(dict(setup=str(executable)))
                self.assertEqual(next(tools.setup_candidates()), executable)
                with patch.object(tools, 'setup_candidates', return_value=iter((missing, executable))):
                    self.assertEqual(tools.find_setup(), executable.resolve())
