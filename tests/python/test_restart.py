"""Remote restart recovery, including ambiguous acknowledgements and routing."""
import struct
import unittest
from unittest.mock import Mock, patch

from test_management import Clock, Serial
from kms_update import management as m


class RestartTests(unittest.TestCase):
    def fixture(self, *, master_cap=16, receiver_cap=16, lost_ack=False, status=m.OK,
                returns=True, changed_image=False, confirmed=True):
        clock, requests = Clock(), []
        def handle(command, count):
            words = command.decode().split()
            local = words[0] == 'master'
            q = m.Frame.decode(bytes.fromhex(words[1 if local else 2]))
            requests.append(q)
            restarted = any(row.op == m.RESTART for row in requests)
            boot = 11 if local else 23 if restarted and returns else 22
            data = b''
            result = m.OK
            if q.op == m.INFO:
                cap = master_cap if local else receiver_cap
                cap |= 4 if local or not restarted or confirmed else 0
                data = m.IDENTITY.pack(b'revision', b'2026-10-29T00:00:00Z',
                    bytes([2 if changed_image and restarted and not local else 1]) * 32, 256,
                    9 if local else 2, cap, 1)
            elif q.op == m.RESTART:
                self.assertEqual(q.target, 22)
                self.assertEqual(q.data, struct.pack('<I', m.RESTART_KEY))
                if lost_ack: return []
                boot, result = 22, status
            r = m.Frame(q.op, q.slot, q.request_id, sender=boot, flags=m.REPLY, data=data, status=result)
            prefix = 'KMSMASTER' if local else f'KMSM {q.slot}'
            return [f'{prefix} {r.encode().hex()}\n'.encode()]
        serial = Serial(handle, clock)
        controller = m.Controller(serial, timeout=.06, clock=clock)
        return controller, clock, requests

    def test_acknowledged_and_lost_ack_restarts_verify_once(self):
        for lost in (False, True):
            with self.subTest(lost_ack=lost):
                controller, clock, requests = self.fixture(lost_ack=lost)
                with patch.object(m.time, 'sleep', side_effect=clock.advance):
                    row = controller.restart(2, timeout=.5)
                self.assertEqual((row['before'], row['after']), ('00000016', '00000017'))
                self.assertEqual(sum(q.op == m.RESTART for q in requests), 1)

    def test_old_master_or_receiver_requires_update_without_restarting(self):
        for caps in ({'master_cap': 0}, {'receiver_cap': 0}):
            controller, _, requests = self.fixture(**caps)
            with self.assertRaises(m.ManagementError) as error: controller.restart(2)
            self.assertEqual(error.exception.code, m.UNSUPPORTED)
            self.assertFalse(any(q.op == m.RESTART for q in requests))

    def test_receiver_restarting_before_its_reply_is_verified(self):
        controller, clock, requests = self.fixture(status=m.WRONG_BOOT)
        with patch.object(m.time, 'sleep', side_effect=clock.advance):
            self.assertEqual(controller.restart(2)['result'], 'restarted')
        self.assertEqual(sum(q.op == m.RESTART for q in requests), 1)

    def test_busy_rejection_never_claims_a_reboot(self):
        controller, _, requests = self.fixture(status=m.BUSY)
        with self.assertRaises(m.ManagementError) as error: controller.restart(2)
        self.assertEqual(error.exception.code, m.BUSY)
        self.assertEqual(sum(q.op == m.INFO for q in requests), 2)

    def test_same_boot_and_unconfirmed_boot_time_out_without_second_restart(self):
        for options in ({'returns': False}, {'confirmed': False}):
            controller, clock, requests = self.fixture(**options)
            with patch.object(m.time, 'sleep', side_effect=clock.advance):
                with self.assertRaisesRegex(m.ManagementError, 'new confirmed boot'):
                    controller.restart(2, timeout=.5)
            self.assertEqual(sum(q.op == m.RESTART for q in requests), 1)

    def test_changed_firmware_is_not_a_successful_restart(self):
        controller, clock, _ = self.fixture(changed_image=True)
        with patch.object(m.time, 'sleep', side_effect=clock.advance):
            with self.assertRaisesRegex(ValueError, 'different firmware'): controller.restart(2)

    def test_batch_selected_last_partial_failure_restores_input(self):
        controller = Mock()
        controller.peers.return_value = ([1, 2, 3], 2)
        calls = []
        def restart(slot, *, before_restart):
            calls.append(slot); before_restart()
            if slot == 3: raise m.ManagementError(m.TIMEOUT)
            return dict(slot=slot, result='restarted')
        controller.restart.side_effect = restart
        report = m.restart_targets(controller, [2, 1, 3, 1])
        self.assertEqual(calls, [1, 3, 2])
        self.assertEqual([row['result'] for row in report['results']], ['restarted', 'unreachable', 'restarted'])
        self.assertFalse(report['success'])
        self.assertEqual(controller.route.call_args_list, [((True,),), ((False,),)])

    def test_already_paused_and_intervening_user_selection_are_preserved(self):
        for current in (0, 3):
            controller = Mock()
            controller.peers.side_effect = [([1, 2, 3], 2), ([1, 2, 3], current)]
            controller.restart.side_effect = lambda slot, before_restart: (before_restart() or dict(slot=slot, result='restarted'))
            self.assertTrue(m.restart_targets(controller, [2])['success'])
            controller.route.assert_not_called()

    def test_failed_pause_still_attempts_resume_and_reports_restoration_failure(self):
        controller = Mock(); controller.peers.return_value = ([2], 2)
        controller.restart.side_effect = lambda slot, before_restart: before_restart()
        controller.route.side_effect = OSError('lost route reply')
        report = m.restart_targets(controller, [2])
        self.assertEqual(controller.route.call_count, 2)
        self.assertIn('restore input', report['warnings'][0])
        self.assertFalse(report['success'])

    def test_unpaired_and_empty_targets_do_not_send_restart(self):
        controller = Mock(); controller.peers.return_value = ([2], 2)
        for targets in ([], [0], [3], [True]):
            with self.assertRaises(ValueError): m.restart_targets(controller, targets)
        controller.restart.assert_not_called()


if __name__ == '__main__': unittest.main()
