import importlib.util
import os
import unittest
from unittest.mock import Mock

from kms_companion.protocol import Message, Op, words
from kms_companion.radio_channel import request_channel
from kms_companion.transport import DeviceError, RejectedCommand


class ChannelTests(unittest.TestCase):
    def test_status_and_set_use_shared_receiver(self):
        receiver = Mock()
        receiver.request.return_value = Message(Op.CHANNEL_SET, data=words(6, 11, 1, 13, 2, 2))
        status = request_channel(receiver, 'channel-set', {'channel': 11})
        receiver.request.assert_called_once_with(Op.CHANNEL_SET, words(11))
        self.assertEqual((status['current'], status['pending']), (6, 11))
        request_channel(receiver, 'channel-resume', {})
        receiver.request.assert_called_with(Op.CHANNEL_RESUME, b'')

    def test_invalid_values_never_reach_device(self):
        receiver = Mock()
        for channel in (0, 15, True, '6', None):
            with self.assertRaises(ValueError):
                request_channel(receiver, 'channel-set', {'channel': channel})
        receiver.request.assert_not_called()

    def test_upgrade_and_lost_reply_are_actionable(self):
        receiver = Mock()
        receiver.request.side_effect = RejectedCommand('unsupported firmware')
        with self.assertRaisesRegex(DeviceError, 'every paired receiver'):
            request_channel(receiver, 'channel-set', {'channel': 11})
        receiver.request.side_effect = DeviceError('Master did not acknowledge command')
        with self.assertRaisesRegex(DeviceError, 'Refresh status before retrying'):
            request_channel(receiver, 'channel-get', {})
        receiver.request.side_effect = RejectedCommand('busy')
        with self.assertRaisesRegex(RejectedCommand, 'busy'):
            request_channel(receiver, 'channel-set', {'channel': 11})

    def test_status_must_respect_country_and_capability_counts(self):
        receiver = Mock()
        receiver.request.return_value = Message(Op.CHANNEL_GET, data=words(6, 11, 1, 11, 1, 2))
        with self.assertRaises(ValueError):
            request_channel(receiver, 'channel-get', {})

    def test_recovery_settings_and_trial_status_round_trip(self):
        receiver = Mock()
        reply = Message(Op.CHANNEL_RECOVERY_SET, data=bytes((1, 3, 20, 2, 0, 6, 13, 2, 1, 1, 1, 2)))
        receiver.request.return_value = Message.decode(reply.encode())
        status = request_channel(receiver, 'channel-recovery-set', {'enabled': True, 'single_peer': True, 'cooldown': 20})
        receiver.request.assert_called_once_with(Op.CHANNEL_RECOVERY_SET, b'\x01\x03\x14')
        self.assertTrue(status['enabled'] and status['single_peer'])
        self.assertEqual((status['phase'], status['expected'], status['attempts']), (2, 1, 1))
        request_channel(receiver, 'channel-recovery-set', {'enabled': False, 'single_peer': True, 'cooldown': 5})
        receiver.request.assert_called_with(Op.CHANNEL_RECOVERY_SET, b'\x01\x02\x05')
        request_channel(receiver, 'channel-recovery-get', {})
        receiver.request.assert_called_with(Op.CHANNEL_RECOVERY_GET, b'')

    def test_recovery_settings_reject_malformed_requests_and_replies(self):
        receiver = Mock()
        valid = dict(enabled=True, single_peer=True, cooldown=20)
        for field, value in (('enabled', 1), ('single_peer', None), ('cooldown', True),
                             ('cooldown', 4), ('cooldown', 61), ('cooldown', '20')):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                request_channel(receiver, 'channel-recovery-set', dict(valid, **{field: value}))
        receiver.request.assert_not_called()
        packet = bytes((1, 2, 15, 0, 0, 0, 0, 0, 0, 0, 0, 2))
        malformed = [b'', packet + b'\x01']
        for index, value in enumerate((2, 4, 4, 5, 8, 14, 14, 3, 128, 1, 3, 3)):
            malformed.append(packet[:index] + bytes((value,)) + packet[index + 1:])
        for data in malformed:
            with self.subTest(data=data), self.assertRaises(ValueError):
                receiver.request.return_value = Message(Op.CHANNEL_RECOVERY_GET, data=data)
                request_channel(receiver, 'channel-recovery-get', {})

    def test_recovery_errors_are_distinct_from_manual_channel_controls(self):
        receiver = Mock()
        receiver.request.side_effect = RejectedCommand('unsupported firmware')
        with self.assertRaisesRegex(DeviceError, 'master and local dongle'):
            request_channel(receiver, 'channel-recovery-get', {})
        receiver.request.side_effect = DeviceError('Radio unavailable')
        with self.assertRaisesRegex(DeviceError, 'refresh before retrying'):
            request_channel(receiver, 'channel-recovery-get', {})
        receiver.request.side_effect = RejectedCommand('storage failure')
        with self.assertRaisesRegex(RejectedCommand, 'storage failure'):
            request_channel(receiver, 'channel-recovery-set', dict(enabled=False, single_peer=True, cooldown=15))


@unittest.skipUnless(importlib.util.find_spec('PySide6'), 'Optional UI dependencies not installed')
class ChannelUiTests(unittest.TestCase):
    def test_recovery_settings_use_radio_tab_and_manual_controls_still_work(self):
        os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
        from PySide6.QtWidgets import QApplication
        from kms_companion.radio_channel_ui import create_dialog
        app = QApplication.instance() or QApplication([])
        submit = Mock()
        dialog = create_dialog(None, submit)
        try:
            dialog.load()
            self.assertEqual(submit.call_args.args, ('channel-get',))
            channel = dict(current=6, pending=6, first=1, count=13, paired=2, supported=2)
            submit.call_args.kwargs['callback'](channel)
            self.assertEqual(submit.call_args.args, ('channel-recovery-get',))
            dialog.show_error('Upgrade firmware', 'channel-recovery-get')
            self.assertTrue(dialog.save.isEnabled() and dialog.resume.isEnabled())
            self.assertFalse(dialog.recovery_save.isEnabled())
            dialog.request_recovery('channel-recovery-get')
            state = dict(enabled=False, single_peer=True, cooldown=15, phase=0, result=0,
                         old=0, candidate=0, expected=0, attempts=0, limit=2)
            submit.call_args.kwargs['callback'](state)
            self.assertTrue(dialog.recovery_save.isEnabled())
            dialog.recovery_enabled.setChecked(True)
            dialog.single_peer.setChecked(False)
            dialog.cooldown.setValue(27)
            dialog.recovery_save.click()
            self.assertEqual(submit.call_args.args, ('channel-recovery-set',))
            self.assertEqual({k: v for k, v in submit.call_args.kwargs.items() if k != 'callback'},
                             dict(enabled=True, single_peer=False, cooldown=27))
            self.assertFalse(dialog.recovery_save.isEnabled())
            submit.call_args.kwargs['callback'](dict(state, enabled=True, phase=2, old=6, candidate=13, attempts=1))
            self.assertIn('Verifying', dialog.recovery_status.text())
            self.assertFalse(dialog.resume.isEnabled())
            self.assertTrue(dialog.save.isEnabled() and dialog.recovery_save.isEnabled())
            dialog.recovery_enabled.setChecked(False)
            dialog.refresh_after_switch()
            submit.call_args.kwargs['callback'](channel)
            self.assertEqual(submit.call_args.args, ('channel-get',))
            self.assertFalse(dialog.recovery_enabled.isChecked())
            dialog.recovery_save.click()
            self.assertIs(submit.call_args.kwargs['enabled'], False)
        finally:
            dialog.timer.stop()
            dialog.close()

    def test_switch_requires_supported_receivers_and_explicit_resume(self):
        os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
        from PySide6.QtWidgets import QApplication
        from kms_companion.radio_channel_ui import create_dialog
        app = QApplication.instance() or QApplication([])
        submit = Mock()
        dialog = create_dialog(None, submit)
        try:
            status = dict(current=6, pending=6, first=1, count=11, paired=2, supported=1)
            dialog.show_state(status)
            self.assertEqual(dialog.channels.count(), 11)
            self.assertFalse(dialog.save.isEnabled())
            dialog.show_state(dict(status, supported=2))
            dialog.channels.setCurrentIndex(10)
            dialog.change()
            self.assertEqual(submit.call_args.args, ('channel-set',))
            self.assertEqual(submit.call_args.kwargs['channel'], 11)
            dialog.show_state(dict(status, supported=2, pending=11), 'channel-set')
            self.assertFalse(dialog.resume.isEnabled())
            dialog.show_state(dict(status, supported=2, current=11, pending=11))
            self.assertTrue(dialog.resume.isEnabled())
            self.assertFalse(any(call.args[0] == 'channel-resume' for call in submit.call_args_list))
            dialog.show_error('Disconnected; refresh')
            self.assertFalse(dialog.save.isEnabled())
            self.assertTrue(dialog.refresh.isEnabled())
        finally:
            dialog.timer.stop()
            dialog.close()
