import json
import os
import tempfile
import unittest
from unittest.mock import Mock, patch
from kms_companion.diagnostics import JournalReader, RecoveryNotices, decode_event
from kms_companion.protocol import Message, Op, words
from kms_companion.transport import DeviceError
from kms_companion.agent import Agent


def event(index=0, **changes):
    values = dict(schema=1, boot=22, token=77, index=index, sequence=index + 1, event_boot=11,
                  first=100, last=150, detail=0xa7000000, repeats=2, code=10, count=2)
    values.update(changes)
    return words(*values.values())


def outcome(sequence=0, kind=0, **changes):
    values = dict(schema=1, master=99, sequence=sequence, kind=kind, slot=2, at=100, age=1,
                  boot=22, reasons=5, attempts=1, selected=2, reserved=0)
    values.update(changes)
    return words(*values.values())


class LocalLogsTests(unittest.TestCase):
    def receiver(self):
        receiver = Mock()
        def request(op, data=b'', **kwargs):
            if op == Op.LOG_BEGIN: return Message(op, data=words(1, 22, 77, 2, 256, 13, 24, 2, 1000, 0, 3, 0))
            if op == Op.LOG_READ: return Message(op, data=event(int.from_bytes(data[4:8], 'little')))
            return Message(op)
        receiver.request.side_effect = request
        return receiver

    def test_incremental_frozen_read_preserves_previous_boots_and_closes_snapshot(self):
        receiver = self.receiver(); reader = JournalReader(); reader.start(receiver)
        for _ in range(4): reader.tick(receiver)
        self.assertFalse(reader.state['loading']); self.assertFalse(reader.state['error'])
        self.assertEqual(reader.state['overwritten'], 13)
        self.assertEqual([r['boot'] for r in reader.state['entries']], ['0000000b'] * 2)
        self.assertIn('Automatic restart', reader.state['entries'][0]['description'])
        self.assertEqual([c.args[0] for c in receiver.request.call_args_list], [Op.LOG_BEGIN, Op.LOG_READ, Op.LOG_READ, Op.LOG_END])

    def test_corrupt_or_stale_records_are_rejected(self):
        for changes in (dict(boot=23), dict(token=78), dict(index=1), dict(count=3), dict(code=16), dict(repeats=0)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                decode_event(event(**changes), 22, 77, 0, 2)

    def test_disconnect_and_partial_read_failure_are_explicit(self):
        receiver = self.receiver(); reader = JournalReader(); reader.start(receiver)
        reader.tick(receiver); reader.tick(receiver)
        receiver.request.side_effect = DeviceError('USB disconnected')
        reader.tick(receiver)
        self.assertFalse(reader.state['loading']); self.assertEqual(len(reader.state['entries']), 1)
        self.assertEqual(reader.state['error'], 'USB disconnected')
        reader.start(receiver); reader.tick(Mock())
        self.assertIn('reconnected', reader.state['error'])

    def test_invalid_metadata_and_empty_journals(self):
        receiver = self.receiver(); reader = JournalReader()
        receiver.request.side_effect = None
        receiver.request.return_value = Message(Op.LOG_BEGIN, data=words(1, 22, 77, 257, 256, *([0] * 7)))
        reader.start(receiver); reader.tick(receiver); self.assertIn('Invalid', reader.state['error'])
        receiver.request.return_value = Message(Op.LOG_BEGIN, data=words(1, 22, 77, 0, 256, *([0] * 7)))
        reader.start(receiver); reader.tick(receiver); reader.tick(receiver)
        self.assertFalse(reader.state['loading']); self.assertFalse(reader.state['error'])


class RecoveryNoticeTests(unittest.TestCase):
    def test_quiet_baseline_deduplication_and_separate_information_warning_limits(self):
        policy = RecoveryNotices(); policy.observe(outcome(), 0)
        policy.observe(outcome(1, 1), 1); policy.observe(outcome(1, 1), 2)
        self.assertEqual(len(policy.events), 1); self.assertTrue(policy.events[-1]['notify'])
        policy.observe(outcome(2, 1), 20); self.assertFalse(policy.events[-1]['notify'])
        policy.observe(outcome(3, 2), 21); self.assertTrue(policy.events[-1]['notify'])
        policy.observe(outcome(4, 2), 22); self.assertFalse(policy.events[-1]['notify'])
        policy.observe(outcome(5, 1), 602); self.assertTrue(policy.events[-1]['notify'])

    def test_muted_events_are_consumed_and_old_events_do_not_replay(self):
        policy = RecoveryNotices(); policy.observe(outcome(5, 1), 0)
        self.assertFalse(policy.events)
        policy.observe(outcome(6, 2), 1, enabled=False); self.assertFalse(policy.events[-1]['notify'])
        policy.observe(outcome(6, 2), 2, enabled=True); self.assertEqual(len(policy.events), 1)
        policy.observe(outcome(7, 2, age=60001), 3); self.assertEqual(len(policy.events), 1)
        policy.observe(outcome(6, 2), 4); self.assertEqual(len(policy.events), 1)
        policy.observe(outcome(1, 1, master=100), 5); self.assertEqual(len(policy.events), 1)

    def test_recovery_setting_survives_auto_toggle_and_helper_restart(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory):
            agent = Agent(); agent.execute('recovery-notifications', dict(enabled=False))
            agent.execute('auto', dict(enabled=False))
            restored = Agent()
            self.assertFalse(restored.recovery_notifications); self.assertFalse(restored.enabled)
            self.assertFalse(json.loads(agent.local_file.read_text())['recovery_notifications'])

    def test_local_log_reader_progresses_while_master_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory):
            agent = Agent(); agent.receiver = Mock(diagnostics=True)
            agent.receiver.status.return_value = dict(diagnostics=True, slot=2, boot=22, master_boot=0, active=False)
            agent.receiver.heartbeat.side_effect = DeviceError('Radio unavailable')
            agent.desktop_error = ''; agent.next_heartbeat = 0
            agent.journal.tick = Mock(); agent.step()
            self.assertTrue(agent.status()['connected']); self.assertFalse(agent.status()['master_connected'])
            agent.step(); self.assertEqual(agent.journal.tick.call_count, 2)


if __name__ == '__main__': unittest.main()
