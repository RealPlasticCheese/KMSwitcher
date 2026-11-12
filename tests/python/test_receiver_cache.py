"""Exercise real cache artifacts and both host cache transfer workflows."""
import base64
import copy
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'companion'))
from kms_update import cache, management as m
from kms_update.__main__ import execute


def receiver_firmware():
    data = bytearray(512)
    data[0], data[12], data[23] = 0xe9, 2, 1
    data[32:36] = bytes.fromhex('3254cdab')
    data[80:100] = b'kmswitcher_receiver\0'
    data += hashlib.sha256(data).digest()
    image = bytes(data)
    metadata = dict(schema=1, role='receiver', chip=2, git='a' * 40, utc='2026-10-28T00:00:00Z',
        size=len(image), sha256=hashlib.sha256(image).hexdigest(), management_protocol=1, input_protocol=2, ota_schema=1)
    return image, metadata


class CacheController:
    def __init__(self, image, metadata):
        self.image, self.metadata = image, metadata
        self.current, self.received, self.operations = None, bytearray(), []
        self.now, self.states, self.wrong_offset = 0, [3, 6, 7], False

    def clock(self): return self.now
    def sleep(self, seconds): self.now += seconds
    def identity(self, slot):
        return dict(chip=9 if slot == 0 else 2, capabilities=15, sha256='0' * 64, boot='1', confirmed=True)

    def request(self, slot, op, data=b'', offset=0, **options):
        self.operations.append(op)
        status, reply = m.OK, b''
        if op == m.CACHE_INFO:
            status = m.OK if self.current else m.EXPIRED
            reply = cache.encode_identity(self.current) if self.current else b''
        elif op == m.CACHE_BEGIN:
            reply = struct.pack('<I', 0)
        elif op == m.CACHE_WRITE:
            if offset != len(self.received): raise AssertionError('Out-of-order upload')
            self.received.extend(data[4:])
            reply = struct.pack('<I', len(self.received) + int(self.wrong_offset))
        elif op == m.CACHE_END:
            if self.received != self.image: raise AssertionError('Wrong cached image')
            self.current = self.metadata
        elif op == m.CACHE_INSTALL:
            reply = struct.pack('<I', 123)
        elif op == m.CACHE_PROGRESS:
            state = self.states.pop(0) if self.states else 9
            reply = struct.pack('<6I', 123, state, m.INTEGRITY if state == 9 else 0,
                                len(self.image), len(self.image), 3)
        return SimpleNamespace(status=status, data=reply)


class CacheTests(unittest.TestCase):
    def test_setup_stores_a_verified_local_image_without_updating_dongles(self):
        image, metadata = receiver_firmware()
        controller = CacheController(image, metadata)
        class SetupController:
            def __new__(cls, connection, *, speed_profile):
                return controller
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'kmswitcher_receiver.bin'
            path.write_bytes(image); path.with_suffix('.json').write_text(json.dumps(metadata))
            args = SimpleNamespace(command='store-receiver', port='fixture', application=path)
            with patch('kms_update.__main__.open_controller'), patch.object(m, 'Controller', SetupController):
                result = execute(args)
            self.assertEqual(result['receiver_cache']['sha256'], metadata['sha256'])
            self.assertIn(m.CACHE_END, controller.operations)
            self.assertNotIn(m.CACHE_INSTALL, controller.operations)
            self.assertNotIn(m.ACTIVATE, controller.operations)

    def test_setup_rejects_wrong_local_role_before_opening_a_port(self):
        image, metadata = receiver_firmware()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'kmswitcher_receiver.bin'
            path.write_bytes(image); path.with_suffix('.json').write_text(json.dumps(metadata))
            args = SimpleNamespace(command='ota', role='controller', port='fixture', application=path)
            with patch('kms_update.__main__.open_controller') as opened:
                with self.assertRaisesRegex(ValueError, 'controller firmware image'):
                    execute(args)
                opened.assert_not_called()
    def setUp(self): self.image, self.metadata = receiver_firmware()

    def test_generated_blob_matches_native_decoder_and_rejects_damage(self):
        blob = cache.image_blob(self.image, self.metadata)
        self.assertEqual(cache.validate_blob(blob), self.metadata)
        library = ROOT / 'build/native-tests/kms-tests.dll'
        if library.exists():
            decode = ctypes.CDLL(str(library)).kms_cache_test_decode
            decode.argtypes = [ctypes.c_void_p]; decode.restype = ctypes.c_uint
            self.assertEqual(decode(ctypes.create_string_buffer(blob[:144])), len(self.image))
        for offset in (0, 4, 12, 80, 136, 140, 4096, 4200):
            damaged = bytearray(blob); damaged[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError): cache.validate_blob(damaged)

    def test_bundle_build_preloads_matching_image_and_ota_manifest(self):
        spec = importlib.util.spec_from_file_location('build_cache', ROOT / 'scripts/build-receiver-cache.py')
        build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
        with tempfile.TemporaryDirectory() as directory:
            controller, receiver = Path(directory) / 'controller', Path(directory) / 'receiver'
            controller.mkdir(); receiver.mkdir()
            master = dict(role='controller', git=self.metadata['git'])
            path = controller / 'kmswitcher_controller.json'; path.write_text(json.dumps(master))
            (receiver / 'kmswitcher_receiver.json').write_text(json.dumps(self.metadata))
            (receiver / 'kmswitcher_receiver.bin').write_bytes(self.image)
            build.build(controller, receiver)
            self.assertEqual(cache.bundled_receiver(json.loads(path.read_text())), (self.image, self.metadata))
            self.assertEqual(cache.validate_blob((controller / 'receiver-cache.bin').read_bytes()), self.metadata)
            self.assertEqual((controller / 'receiver-cache-empty.bin').read_bytes(), b'\xff' * 4096)
            master['git'] = 'b' * 40; path.write_text(json.dumps(master))
            with self.assertRaisesRegex(ValueError, 'same source'): build.build(controller, receiver)

    def test_embedded_payload_wrong_chip_project_hash_and_revision_are_rejected(self):
        master = dict(role='controller', git=self.metadata['git'], receiver_firmware=dict(metadata=self.metadata,
            image_base64=base64.b64encode(self.image).decode()))
        self.assertEqual(cache.bundled_receiver(master), (self.image, self.metadata))
        for key, value in (('chip', 9), ('sha256', '0' * 64), ('git', 'b' * 40)):
            bad = copy.deepcopy(master); bad['receiver_firmware']['metadata'][key] = value
            with self.assertRaises(ValueError): cache.bundled_receiver(bad)
        bad = bytearray(self.image); bad[80] = 0
        bad[-32:] = hashlib.sha256(bad[:-32]).digest()
        metadata = dict(self.metadata, sha256=hashlib.sha256(bad).hexdigest())
        with self.assertRaises(ValueError): cache.validate_receiver(bad, metadata)

    def test_staging_validates_offsets_verifies_commit_and_skips_matching_cache(self):
        controller = CacheController(self.image, self.metadata)
        self.assertEqual(m.stage_cache(controller, self.image, self.metadata), 'updated')
        count = controller.operations.count(m.CACHE_WRITE)
        self.assertEqual(m.stage_cache(controller, self.image, self.metadata), 'skipped')
        self.assertEqual(controller.operations.count(m.CACHE_WRITE), count)
        controller.current = None; controller.received.clear(); controller.wrong_offset = True
        with self.assertRaises(m.ManagementError): m.stage_cache(controller, self.image, self.metadata)
        self.assertIn(m.CACHE_ABORT, controller.operations)

    def test_cached_update_only_polls_progress_and_requires_confirmation(self):
        controller = CacheController(self.image, self.metadata)
        pauses, progress = [], []
        self.assertEqual(m.update_cached_one(controller, 3, self.metadata, before_write=lambda: pauses.append(True),
            progress=lambda done, total: progress.append(done), sleep=controller.sleep), 'updated')
        self.assertEqual(pauses, [True]); self.assertEqual(len(progress), 3)
        self.assertNotIn(m.WRITE, controller.operations)
        self.assertNotIn(m.CACHE_WRITE, controller.operations)
        controller.states = [9]
        with self.assertRaisesRegex(m.ManagementError, 'integrity'):
            m.update_cached_one(controller, 3, self.metadata, sleep=controller.sleep)

    def test_cached_and_file_sources_are_mutually_exclusive(self):
        self.assertTrue(m.parser().parse_args(['update', '--all', '--cached']).cached)
        self.assertEqual(m.parser().parse_args(['cache-status']).operation, 'cache-status')
        with self.assertRaises(SystemExit): m.parser().parse_args(['update', '--all', '--cached', '--firmware', 'file.bin'])
