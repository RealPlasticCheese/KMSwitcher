"""Hardware-free checks of the Background preset's read-only cache load."""
import struct
import unittest
from unittest.mock import Mock
import zlib

from kms_stress.management import HEADER, inspect_cache, inspect_radio, management_request


class Channel:
    def __init__(self, *, status=0, guard='KMSTRANSFER busy=0', corrupt=False, stale=False,
                 radio=False, deferred=True, enabled=True):
        self.commands, self.status, self.guard, self.corrupt, self.stale = [], status, guard, corrupt, stale
        self.radio, self.deferred, self.enabled = radio, deferred, enabled

    def request(self, command, prefix, service=None):
        self.commands.append(command)
        if service: service()
        if command == 'transfer-status':
            if self.guard is None: raise TimeoutError('No guard support')
            return [self.guard]
        if command == 'receiver-recovery':
            return [f'KMSRECOVERY enabled={int(self.enabled)} test_deferred={int(self.deferred)}']
        request = bytes.fromhex(command.split()[-1])
        fields = HEADER.unpack_from(request)
        op, request_id = fields[2], fields[6]
        result = 0 if op == 1 else 9 if op == 19 else self.status
        if op == 1: data = bytes(112) + struct.pack('<II', 32 if self.radio else 0, 1)
        elif op == 21: data = struct.pack('<20I', 1, 22, *range(2, 20))
        else: data = bytes(120) if result == 0 else b''
        body = HEADER.pack(b'KMSM', 1, op, 1, result, fields[5],
            request_id ^ int(self.stale), 22, 99 if command.startswith('remote ') else 0, 0, len(data)) + data
        response = bytearray(body + struct.pack('<I', zlib.crc32(body)))
        if self.corrupt: response[-1] ^= 1
        return [prefix + response.hex()]


class ManagementLoadTests(unittest.TestCase):
    def test_queries_are_read_only_boot_bound_and_require_a_clear_guard(self):
        for result in (0, 7, 9):
            channel, service = Channel(status=result), Mock()
            self.assertEqual(inspect_cache(channel, service), dict(cache_status=result, progress_status=9, update_guard_clear=True))
            frames = [HEADER.unpack_from(bytes.fromhex(command.split()[1])) for command in channel.commands[:3]]
            self.assertEqual([frame[2] for frame in frames], [1, 13, 19])
            self.assertEqual([frame[8] for frame in frames], [0, 22, 22])
            self.assertEqual(channel.commands[-1], 'transfer-status')
            self.assertEqual(service.call_count, 4)

    def test_a_lingering_guard_is_a_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'handover update guard blocked'):
            inspect_cache(Channel(guard='KMSTRANSFER busy=1'))

    def test_old_firmware_cannot_claim_this_regression_passed(self):
        with self.assertRaisesRegex(RuntimeError, 'Update S3 test firmware'):
            inspect_cache(Channel(guard=None))

    def test_corrupt_stale_and_rejected_replies_fail(self):
        for options in ({'corrupt': True}, {'stale': True}, {'status': 2}):
            with self.subTest(options=options), self.assertRaises(RuntimeError): inspect_cache(Channel(**options))

    def test_receiver_radio_fields_are_boot_bound_and_test_recovery_is_deferred(self):
        channel = Channel(radio=True)
        sample = inspect_radio(channel, 2)
        self.assertEqual((sample['boot'], sample['tx_failed'], sample['max_rx_gap_ms']), ('00000016', 5, 13))
        self.assertTrue(sample['supported'] and sample['recovery_test_deferred'])
        self.assertTrue(channel.commands[0].startswith('remote 2 '))
        self.assertEqual(channel.commands[-1], 'receiver-recovery')

    def test_radio_diagnostics_are_explicitly_unsupported_on_old_receivers(self):
        channel = Channel()
        self.assertFalse(inspect_radio(channel, 2)['supported'])
        self.assertEqual(len(channel.commands), 1)

    def test_stress_cannot_run_with_automatic_restart_enabled_without_deferral(self):
        with self.assertRaisesRegex(RuntimeError, 'must be deferred'):
            inspect_radio(Channel(radio=True, deferred=False), 2)
        self.assertFalse(inspect_radio(Channel(radio=True, deferred=False, enabled=False), 2)['recovery_enabled'])

    def test_inspection_probe_cannot_issue_mutating_operations(self):
        channel = Channel()
        for op in (6, 7, 9, 18, 20):
            with self.assertRaises(ValueError): management_request(channel, 2, op)
        self.assertEqual(channel.commands, [])


if __name__ == '__main__': unittest.main()
