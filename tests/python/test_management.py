"""Host workflow tests with serial loss/corruption and fake receiver responses."""
import hashlib
import ctypes
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'companion'))
from kms_update import management as m
spec = importlib.util.spec_from_file_location('build_metadata', ROOT / 'scripts/build-metadata.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


def identity_bytes(digest, confirmed=True):
    return m.IDENTITY.pack(b'abcdef-dirty', b'2026-10-17T00:00:00Z', bytes.fromhex(digest),
                           256, 2, 3 | (4 if confirmed else 0), 1)


class Clock:
    def __init__(self): self.now = 0
    def __call__(self): return self.now
    def advance(self, seconds): self.now += seconds


class Serial:
    def __init__(self, handler, clock):
        self.handler, self.clock = handler, clock
        self.sent, self.pending = [], []
    def write(self, wire):
        self.sent.append(wire)
        self.pending.extend(self.handler(wire, len(self.sent)))
    def flush(self): pass
    def readline(self, size):
        self.clock.advance(0.02)
        return self.pending.pop(0) if self.pending else b''


def reply(command, data=b'', status=0, sender=22):
    request = m.Frame.decode(bytes.fromhex(command.decode().split()[2]))
    return m.Frame(request.op, request.slot, request.request_id, sender=sender, target=11,
                   offset=request.offset, flags=m.REPLY, status=status, data=data)


def line(frame):
    return f'KMSM {frame.slot} {frame.encode().hex()}\n'.encode()


class SerialProtocolTests(unittest.TestCase):
    def test_c_firmware_and_python_wire_formats_agree(self):
        library = ROOT / 'build/native-tests/kms-tests.dll'
        if not library.exists(): self.skipTest('Run scripts/test-core.py for C/Python interoperability coverage')
        echo = ctypes.CDLL(str(library)).kms_management_test_echo
        echo.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p]
        echo.restype = ctypes.c_uint
        for size in (0, 1, 4, 120, 168, 192):
            request = m.Frame(m.WRITE, 3, 123, sender=456, target=789, offset=101,
                              data=bytes(range(size))).encode()
            source, destination = ctypes.create_string_buffer(request), ctypes.create_string_buffer(226)
            count = echo(source, len(request), destination)
            self.assertEqual(destination.raw[:count], request)
            source[20] = bytes([request[20] ^ 1])
            self.assertEqual(echo(source, len(request), destination), 0)

    def test_crc_covers_offsets_and_data(self):
        frame = m.Frame(m.WRITE, 7, 123, sender=456, target=789, offset=101, data=bytes(range(192)))
        wire = frame.encode()
        self.assertEqual(len(wire), 226)
        self.assertEqual(m.Frame.decode(wire), frame)
        for at in range(len(wire)):
            damaged = bytearray(wire)
            damaged[at] ^= 1
            with self.assertRaises(ValueError): m.Frame.decode(damaged)

    def test_lost_reply_retries_the_identical_request(self):
        clock = Clock()
        serial = Serial(lambda command, count: [] if count == 1 else [line(reply(command, b'ok'))], clock)
        controller = m.Controller(serial, timeout=0.1, clock=clock)
        self.assertEqual(controller.request(2, m.STATS).data, b'ok')
        self.assertEqual(serial.sent[0], serial.sent[1])

    def test_corrupt_reply_is_discarded_and_retried(self):
        clock = Clock()
        def handler(command, count):
            response = reply(command, b'ok')
            if count == 1:
                wire = bytearray(response.encode())
                wire[-1] ^= 1
                return [f'KMSM 2 {wire.hex()}\n'.encode()]
            return [line(response)]
        serial = Serial(handler, clock)
        controller = m.Controller(serial, timeout=0.1, clock=clock)
        self.assertEqual(controller.request(2, m.STATS).data, b'ok')
        self.assertEqual(len(serial.sent), 2)

    def test_master_timeout_retries_an_in_progress_slave_flash_operation(self):
        clock = Clock()
        serial = Serial(lambda command, count: [line(reply(command, b'ok' if count > 1 else b'',
                            status=m.TIMEOUT if count == 1 else m.OK))], clock)
        controller = m.Controller(serial, timeout=0.1, clock=clock)
        self.assertEqual(controller.request(2, m.BEGIN).data, b'ok')
        self.assertEqual(serial.sent[0], serial.sent[1])

    def test_write_survives_a_loss_burst_without_changing_id_or_payload(self):
        clock = Clock()
        serial = Serial(lambda command, count: [] if count <= 5 else
                        [line(reply(command, struct.pack('<I', 376)))], clock)
        controller = m.Controller(serial, timeout=0.1, clock=clock)
        response = controller.request(2, m.WRITE, struct.pack('<I', 123) + b'x' * 188, offset=188)
        self.assertEqual(response.data, struct.pack('<I', 376))
        self.assertEqual(len(serial.sent), 6)
        self.assertEqual(len(set(serial.sent)), 1)

    def test_megabyte_upload_recovers_lost_acknowledgements_at_twenty_percent(self):
        clock, flashed, dropped = Clock(), bytearray(), [0]
        image = bytes(range(256)) * 4096
        failure_offset = (len(image) // 5 // 188) * 188
        def handler(command, count):
            request = m.Frame.decode(bytes.fromhex(command.decode().split()[2]))
            chunk = request.data[4:]
            if request.offset == len(flashed):
                flashed.extend(chunk)
            else:
                self.assertEqual(flashed[request.offset:request.offset + len(chunk)], chunk)
            if request.offset == failure_offset and dropped[0] < 5:
                dropped[0] += 1
                return []
            return [line(reply(command, struct.pack('<I', len(flashed))))]
        controller = m.Controller(Serial(handler, clock), timeout=0.1, clock=clock)
        for offset in range(0, len(image), 188):
            chunk = image[offset:offset + 188]
            response = controller.request(2, m.WRITE, struct.pack('<I', 123) + chunk, offset=offset)
            self.assertEqual(response.data, struct.pack('<I', offset + len(chunk)))
        self.assertEqual(flashed, image)
        self.assertEqual(dropped[0], 5)

    def test_write_loss_is_bounded_and_reports_the_failing_offset(self):
        clock = Clock()
        serial = Serial(lambda command, count: [], clock)
        controller = m.Controller(serial, timeout=0.1, clock=clock)
        with self.assertRaisesRegex(m.ManagementError, 'byte 940'):
            controller.request(2, m.WRITE, b'data', offset=940)
        self.assertEqual(len(serial.sent), 8)

    def test_unrelated_and_partial_lines_do_not_satisfy_a_request(self):
        clock = Clock()
        def handler(command, count):
            correct = reply(command, b'ok')
            unrelated = reply(command, b'old')
            unrelated.request_id += 1
            packet = line(correct)
            return [b'noise\n', line(unrelated), packet[:35], packet[35:]]
        controller = m.Controller(Serial(handler, clock), timeout=0.2, clock=clock)
        self.assertEqual(controller.request(2, m.STATS).data, b'ok')

    def test_offline_target_has_a_bounded_timeout(self):
        clock = Clock()
        serial = Serial(lambda command, count: [], clock)
        controller = m.Controller(serial, timeout=0.1, attempts=2, clock=clock)
        with self.assertRaises(m.ManagementError) as raised: controller.request(7, m.INFO)
        self.assertEqual(raised.exception.code, m.TIMEOUT)
        self.assertEqual(len(serial.sent), 2)

    def test_download_checks_snapshot_crc_and_exact_size(self):
        for corrupt in (False, True):
            with self.subTest(corrupt=corrupt):
                clock, text = Clock(), b'retained diagnostic event\n' * 30
                def handler(command, count):
                    request = m.Frame.decode(bytes.fromhex(command.decode().split()[2]))
                    if request.op == m.INFO: data = identity_bytes('aa' * 32)
                    elif request.op == m.HISTORY_BEGIN:
                        data = struct.pack('<III', 99, len(text), zlib.crc32(text) ^ int(corrupt))
                    else:
                        self.assertEqual(request.data, struct.pack('<I', 99))
                        data = text[request.offset:request.offset + 192]
                    return [line(reply(command, data))]
                controller = m.Controller(Serial(handler, clock), timeout=0.2, clock=clock)
                if corrupt:
                    with self.assertRaises(m.ManagementError): controller.history(3)
                else: self.assertEqual(controller.history(3), text)

    def test_large_history_and_eof_release_with_new_and_old_receivers(self):
        for legacy in (False, True):
            clock, content = Clock(), b'event\n' * 12000
            released = []
            def handler(command, count):
                request = m.Frame.decode(bytes.fromhex(command.decode().split()[2]))
                status = m.OK
                if request.op == m.INFO:
                    data = identity_bytes('aa' * 32)
                elif request.op == m.HISTORY_BEGIN:
                    data = struct.pack('<III', 99, len(content), zlib.crc32(content))
                elif request.offset == len(content):
                    released.append(request.offset)
                    data, status = b'', m.OFFSET if legacy else m.OK
                else:
                    data = content[request.offset:request.offset + 192]
                return [line(reply(command, data, status=status))]
            controller = m.Controller(Serial(handler, clock), timeout=0.2, clock=clock)
            self.assertEqual(controller.history(3), content)
            self.assertEqual(released, [len(content)])

    def test_paired_list_distinguishes_selected_slot(self):
        clock = Clock()
        serial = Serial(lambda command, count: [b'KMSPEER 2 00000016\n',
            b'KMSPEER 4 00000023\n', b'KMSROUTE CURRENT 4\n', b'KMSPEERS END\n'], clock)
        self.assertEqual(m.Controller(serial, clock=clock).peers(), ([2, 4], 4))


class FakeController:
    def __init__(self, image, current=False, fail_end=False, rollback=False):
        self.clock = Clock()
        self.image, self.data = image, bytearray()
        self.current, self.fail_end, self.rollback = current, fail_end, rollback
        self.ops, self.identities = [], 0
        self.activated = False
        self.digest = hashlib.sha256(image).hexdigest()
    def identity(self, slot):
        self.identities += 1
        self.clock.advance(0.1)
        rebooted = self.activated and self.identities >= 3
        digest = self.digest if self.current or (rebooted and not self.rollback) else 'bb' * 32
        return dict(chip=2, capabilities=7, sha256=digest, confirmed=not rebooted or self.identities >= 4,
                    boot='00000002' if rebooted else '00000001')
    def request(self, slot, op, data=b'', offset=0, force=False, discover=False, allow=()):
        self.ops.append((op, force))
        response = m.Frame(op, slot, 1, flags=m.REPLY)
        if op == m.BEGIN:
            self.asserted_digest = data[8:40].hex()
            response.data = struct.pack('<I', 0)
        elif op == m.WRITE:
            if offset != len(self.data): raise AssertionError('Host reordered firmware chunks')
            self.data.extend(data[4:])
            response.data = struct.pack('<I', len(self.data))
        elif op == m.END:
            if self.fail_end or hashlib.sha256(self.data).hexdigest() != self.asserted_digest:
                raise m.ManagementError(m.INTEGRITY)
        elif op == m.ACTIVATE: self.activated = True
        return response


class UpdateTests(unittest.TestCase):
    def test_matching_image_is_skipped_without_pausing_or_writing(self):
        image = bytes(range(256))
        controller = FakeController(image, current=True)
        self.assertEqual(m.update_one(controller, 1, image, dict(chip=2, sha256=controller.digest)), 'skipped')
        self.assertEqual(controller.ops, [])

    def test_force_requires_verified_flash_and_a_new_confirmed_boot(self):
        image = bytes(range(256))
        controller = FakeController(image, current=True)
        outcome = m.update_one(controller, 1, image, dict(chip=2, sha256=controller.digest), True,
                               sleep=controller.clock.advance)
        self.assertEqual(outcome, 'updated')
        self.assertGreaterEqual(controller.identities, 4)
        self.assertEqual(controller.ops[0], (m.BEGIN, True))
        self.assertEqual(controller.data, image)
        self.assertEqual([op for op, force in controller.ops][-2:], [m.END, m.ACTIVATE])

    def test_force_does_not_bypass_readback_failure(self):
        image = bytes(range(256))
        controller = FakeController(image, current=True, fail_end=True)
        with self.assertRaises(m.ManagementError):
            m.update_one(controller, 1, image, dict(chip=2, sha256=controller.digest), True,
                         sleep=controller.clock.advance)
        self.assertFalse(controller.activated)
        self.assertEqual(controller.ops[-1][0], m.ABORT)

    def test_rollback_is_reported_as_failure(self):
        image = bytes(range(256))
        controller = FakeController(image, rollback=True)
        with self.assertRaisesRegex(m.ManagementError, 'rolled back'):
            m.update_one(controller, 1, image, dict(chip=2, sha256=controller.digest), sleep=controller.clock.advance)

    def test_all_targets_are_sequential_selected_last_and_failures_do_not_stop_the_batch(self):
        calls = []
        class BatchController:
            def route(self, pause): calls.append(('route', pause))
        def update(controller, slot, image, metadata, force, before_write, progress):
            calls.append(('slot', slot))
            before_write()
            if slot == 3: raise m.ManagementError(m.TIMEOUT)
            return 'updated'
        with patch.object(m, 'update_one', side_effect=update):
            results = m.update_targets(BatchController(), [1, 2, 3], 1, b'image', {}, emit=lambda row: None)
        self.assertEqual([row['slot'] for row in results], [2, 3, 1])
        self.assertEqual(results[1]['result'], 'unreachable')
        self.assertEqual(calls, [('slot', 2), ('slot', 3), ('slot', 1), ('route', True), ('route', False)])

    def test_skipped_selected_target_never_pauses_routing(self):
        class BatchController:
            def route(self, pause): raise AssertionError('A skipped image must not interrupt input')
        with patch.object(m, 'update_one', return_value='skipped'):
            self.assertEqual(m.update_targets(BatchController(), [1], 1, b'', {}, emit=lambda row: None),
                             [dict(slot=1, result='skipped')])

    def test_lost_pause_acknowledgement_still_attempts_route_restoration(self):
        calls = []
        class BatchController:
            def route(self, pause):
                calls.append(pause)
                if pause: raise m.ManagementError(m.TIMEOUT)
        def update(controller, slot, image, metadata, force, before_write, progress):
            before_write()
        with patch.object(m, 'update_one', side_effect=update):
            m.update_targets(BatchController(), [1], 1, b'', {}, emit=lambda row: None)
        self.assertEqual(calls, [True, False])


class BuildTests(unittest.TestCase):
    def test_receiver_manifest_requires_matching_embedded_identity_and_rollback_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, header = root / 'receiver.bin', root / 'identity.h'
            identity = dict(git='a' * 40, utc='2026-10-17T00:00:00Z')
            header.with_suffix('.json').write_text(json.dumps(identity))
            (root / 'config').mkdir()
            config = root / 'config/sdkconfig.json'
            config.write_text(json.dumps(dict(BOOTLOADER_APP_ROLLBACK_ENABLE=False, PARTITION_TABLE_CUSTOM=True)))
            image = bytearray(256)
            image[0], image[12] = 0xe9, 2
            image[64:105] = identity['git'].encode() + b'\0'
            image[128:149] = identity['utc'].encode() + b'\0'
            binary.write_bytes(image)
            with self.assertRaisesRegex(ValueError, 'ResetConfig'): b.manifest(binary, header, 'receiver', 2)
            config.write_text(json.dumps(dict(BOOTLOADER_APP_ROLLBACK_ENABLE=True, PARTITION_TABLE_CUSTOM=True)))
            b.manifest(binary, header, 'receiver', 2)
            self.assertEqual(json.loads(binary.with_suffix('.json').read_text())['ota_schema'], 1)
            image[64] ^= 1
            binary.write_bytes(image)
            with self.assertRaisesRegex(ValueError, 'identity'): b.manifest(binary, header, 'receiver', 2)

    def test_stamp_is_generated_in_utc_without_manual_release_metadata(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(b, 'git_identity', return_value='a' * 40):
            header = Path(directory) / 'kms_build_identity.h'
            b.stamp(ROOT, header)
            metadata = json.loads(header.with_suffix('.json').read_text())
            self.assertEqual(metadata['git'], 'a' * 40)
            self.assertRegex(metadata['utc'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
            self.assertIn(metadata['utc'], header.read_text())
    def test_metadata_and_appended_hash_are_both_required_even_for_force(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'receiver.bin'
            content = bytearray(256)
            content[0], content[12], content[23] = 0xe9, 2, 1
            image = bytes(content) + hashlib.sha256(content).digest()
            metadata = dict(schema=1, role='receiver', chip=2, size=len(image), sha256=hashlib.sha256(image).hexdigest(),
                            management_protocol=1, input_protocol=2, ota_schema=1)
            path.write_bytes(image)
            path.with_suffix('.json').write_text(json.dumps(metadata))
            self.assertEqual(m.load_firmware(path)[0], image)
            damaged = bytearray(image)
            damaged[100] ^= 1
            metadata['sha256'] = hashlib.sha256(damaged).hexdigest()
            path.write_bytes(damaged)
            path.with_suffix('.json').write_text(json.dumps(metadata))
            with self.assertRaisesRegex(m.ManagementError, 'appended'): m.load_firmware(path)

    def test_source_identity_marks_uncommitted_changes(self):
        with patch.object(b.subprocess, 'check_output', side_effect=['a' * 40 + '\n', ' M file.c\n']):
            self.assertEqual(b.git_identity(ROOT), 'a' * 40 + '-dirty')
        with patch.object(b.subprocess, 'check_output', side_effect=['b' * 40 + '\n', '']):
            self.assertEqual(b.git_identity(ROOT), 'b' * 40)

    def test_target_selection_is_required_and_mutually_exclusive(self):
        with patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit): m.parser().parse_args(['update', '--firmware', 'image.bin'])
            with self.assertRaises(SystemExit):
                m.parser().parse_args(['update', '--slot', '1', '--all', '--firmware', 'image.bin'])


if __name__ == '__main__':
    unittest.main()
