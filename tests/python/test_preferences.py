"""C/browser codec agreement and controller OTA bootstrap invariants."""
import csv
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('metadata_preferences_test', ROOT / 'scripts/build-metadata.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

class PreferencesTests(unittest.TestCase):
    def test_browser_preferences_codec_matches_the_production_c_codec(self):
        library = ROOT / 'build/native-tests/kms-tests.dll'
        if not library.exists() or not shutil.which('node'):
            self.skipTest('Run scripts/test-core.py and install Node for C/browser interoperability')
        echo = ctypes.CDLL(str(library)).kms_preferences_test_echo
        echo.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p]
        echo.restype = ctypes.c_uint
        source = "import {encodePreferences} from './web/preferences.js'; process.stdout.write(Buffer.from(encodePreferences(JSON.parse(process.argv[1]))).toString('hex'));"
        for double_ms, select_ms, brightness in [(100, 500, 1), (350, 3000, 8), (1000, 15000, 255)]:
            values = dict(jigglerSeconds=65535, jigglerDistance=127, bootSlot=7, trigger=5, doubleMs=double_ms, selectMs=select_ms, normalLed=1,
                          brightness=brightness, keyboardLeds=0, slotColors=[7, 6, 5, 4, 3, 2, 1])
            wire = bytes.fromhex(subprocess.check_output(['node', '--input-type=module', '-e', source, json.dumps(values)], cwd=ROOT, text=True))
            input_buffer, output = ctypes.create_string_buffer(wire), ctypes.create_string_buffer(32)
            self.assertEqual(echo(input_buffer, len(wire), output), 32)
            self.assertEqual(output.raw, wire)
            input_buffer[7] = bytes([wire[7] ^ 1])
            self.assertEqual(echo(input_buffer, len(wire), output), 0)

    def test_master_manifest_requires_rollback_even_with_a_valid_s3_application(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, header = root / 'controller.bin', root / 'identity.h'
            identity = dict(git='a' * 40, utc='2026-10-17T00:00:00Z')
            header.with_suffix('.json').write_text(json.dumps(identity))
            (root / 'config').mkdir()
            config = root / 'config/sdkconfig.json'
            config.write_text(json.dumps(dict(BOOTLOADER_APP_ROLLBACK_ENABLE=False, PARTITION_TABLE_CUSTOM=True)))
            data = bytearray(256); data[0], data[12] = 0xe9, 9
            data[64:105] = identity['git'].encode() + b'\0'
            data[128:149] = identity['utc'].encode() + b'\0'
            binary.write_bytes(data)
            with self.assertRaisesRegex(ValueError, 'ResetConfig'): build.manifest(binary, header, 'controller', 9)
            config.write_text(json.dumps(dict(BOOTLOADER_APP_ROLLBACK_ENABLE=True, PARTITION_TABLE_CUSTOM=True)))
            build.manifest(binary, header, 'controller', 9)
            manifest = json.loads(binary.with_suffix('.json').read_text())
            self.assertEqual((manifest['role'], manifest['chip'], manifest['ota_schema']), ('controller', 9, 1))
            self.assertEqual(manifest['sha256'], hashlib.sha256(data).hexdigest())

    def test_master_partition_migration_preserves_pairing_phy_and_factory_offsets(self):
        text = (ROOT / 'config/partitions.controller.csv').read_text()
        rows = [row for row in csv.reader(text.splitlines()) if row and not row[0].startswith('#')]
        partitions = {row[0].strip(): (int(row[3].strip(), 0), int(row[4].strip(), 0)) for row in rows}
        self.assertEqual(partitions['nvs'], (0x9000, 0x6000))
        self.assertEqual(partitions['phy_init'], (0xf000, 0x1000))
        self.assertEqual(partitions['factory'], (0x10000, 0x300000))
        self.assertEqual(partitions['otadata'][1], 0x2000)
        ranges = sorted(partitions.values())
        self.assertTrue(all(start + size <= next_start for (start, size), (next_start, _) in zip(ranges, ranges[1:])))
        self.assertTrue(all(start + size <= 16 * 1024 * 1024 for start, size in ranges))
        self.assertEqual(partitions['ota_0'][1], partitions['ota_1'][1])
        self.assertGreaterEqual(partitions['ota_0'][1], 0x300000)

if __name__ == '__main__':
    unittest.main()
