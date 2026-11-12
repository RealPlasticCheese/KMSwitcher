"""Flashing safety checks use a fake ROM/stub; no serial device is opened."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from kms_companion.firmware import Bundle, flash, partitions
from kms_update.cache import image_blob


def table(entries):
    data = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, sub, start, size, name.encode(), 0)
                    for name, kind, sub, start, size in entries)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(4096, b"\xff")


def make_bundle(directory):
    document = dict(schema=1, version="0.1.0", commit="a" * 40, roles={})
    for role, chip, ident, capacity, app, ota, boot in (
        ("controller", "esp32s3", 9, 16 << 20, 0x10000, 0x310000, 0),
        ("receiver", "esp32s2", 2, 4 << 20, 0x20000, 0x10000, 0x1000)):
        image = bytearray(4096); image[0] = 0xE9; image[12:14] = ident.to_bytes(2, "little")
        layout = table([("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
                        ("otadata", 1, 0, ota, 0x2000), ("factory" if role == "controller" else "ota_0", 0,
                                                       0 if role == "controller" else 0x10, app, 0x100000)])
        files = []
        for name, address, data in (("boot.bin", boot, image), ("partitions.bin", 0x8000, layout),
                                    ("ota.bin", ota, b"\xff" * 8192), ("app.bin", app, image)):
            relative = f"{role}/{name}"
            path = directory / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            files.append(dict(path=relative, offset=address, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        document["roles"][role] = dict(chip=chip, capacity=capacity, files=files,
            settings=dict(flash_mode="dio", flash_freq="40m", flash_size="16MB" if role == "controller" else "4MB"))
    (directory / "bundle.json").write_text(json.dumps(document))
    return document


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.document = make_bundle(self.directory)

    def save(self):
        (self.directory / "bundle.json").write_text(json.dumps(self.document))

    def test_both_board_layouts_validate(self):
        self.assertEqual(Bundle(self.directory).validate()["roles"], ["controller", "receiver"])

    def test_checksum_failure_never_opens_a_port(self):
        (self.directory / "receiver/app.bin").write_bytes(b"corrupt")
        with patch("esptool.cmds.detect_chip") as connect:
            with self.assertRaisesRegex(ValueError, "checksum"):
                flash("receiver", "not-a-real-port", self.directory)
            connect.assert_not_called()

    def test_image_for_wrong_chip_rejected_even_with_updated_checksum(self):
        entry = self.document["roles"]["receiver"]["files"][-1]
        path = self.directory / entry["path"]
        data = bytearray(path.read_bytes()); data[12] = 9; path.write_bytes(data)
        entry["sha256"] = hashlib.sha256(data).hexdigest(); self.save()
        with self.assertRaisesRegex(ValueError, "chip"):
            Bundle(self.directory).files("receiver")

    def test_bundle_cannot_reference_outside_files_or_write_pairing_sectors(self):
        original = copy.deepcopy(self.document)
        self.document["roles"]["receiver"]["files"][0]["path"] = "../secret"
        self.save()
        with self.assertRaisesRegex(ValueError, "escapes"):
            Bundle(self.directory).files("receiver")
        self.document = original
        self.document["roles"]["receiver"]["files"][0]["offset"] = 0x9000; self.save()
        with self.assertRaises(ValueError):
            Bundle(self.directory).files("receiver")

    def simulate(self, *, chip="ESP32-S2", exponent=22, old_table=None, expected_mac=None, corrupt_nvs=False, role='receiver', identify=False, erase=False):
        class Board:
            CHIP_NAME = chip
            def __enter__(self): return self
            def __exit__(self, *_): pass
            def read_mac(self): return bytes.fromhex("020000002003")
            def flash_id(self): return exponent << 16
        before = bytes(range(256)) * 96
        wrote = []
        operations = []
        def read(_, address, size):
            if address == 0x8000:
                return old_table if old_table is not None else b"\xff" * size
            return b"\0" * size if wrote and corrupt_nvs else before
        with patch("esptool.cmds.detect_chip", return_value=Board()) as connect, \
             patch("esptool.cmds.run_stub", side_effect=lambda board: board), \
             patch("esptool.cmds.attach_flash"), patch("esptool.cmds.read_flash", side_effect=read) as read_flash, \
             patch("esptool.cmds.erase_flash", side_effect=lambda *_: operations.append("erase")) as erase_flash, \
             patch("esptool.cmds.write_flash", side_effect=lambda *a, **kw: (wrote.append(a[1]), operations.append("write"))) as write, \
             patch("esptool.cmds.verify_flash") as verify, patch("esptool.cmds.reset_chip") as reset:
            try:
                result = flash(role, "fake-port", self.directory, expected_mac=expected_mac,
                               backup_directory=self.directory / "backups", identify=identify, erase=erase)
                if identify:
                    read_flash.assert_not_called()
                    erase_flash.assert_not_called()
                    write.assert_not_called()
                    return result
                if erase:
                    self.assertIsNone(result["backup"])
                    read_flash.assert_not_called()
                    self.assertEqual(operations, ["erase", "write"])
                else:
                    erase_flash.assert_not_called()
                    self.assertEqual(Path(result["backup"]).read_bytes(), before)
                verify.assert_called_once()
                if role == 'controller': reset.assert_called_once()
                else: reset.assert_not_called()
                self.assertEqual(connect.call_args.kwargs["connect_mode"], 'default-reset' if role == 'controller' else 'no-reset')
                return result
            except ValueError:
                erase_flash.assert_not_called(); write.assert_not_called(); verify.assert_not_called()
                raise

    def test_blank_and_existing_board_preserve_settings_and_verify(self):
        self.assertTrue(self.simulate()["verified"])
        self.assertTrue(self.simulate(old_table=(self.directory / "receiver/partitions.bin").read_bytes())["verified"])
        legacy = table([("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
                        ("factory", 0, 0, 0x10000, 0x200000)])
        self.assertTrue(self.simulate(old_table=legacy)["verified"])

    def test_chip_capacity_mac_and_existing_layout_mismatches_do_not_write(self):
        for arguments in (dict(chip="ESP32-S3"), dict(exponent=20), dict(expected_mac="02:00:00:00:00:99"),
                          dict(old_table=table([("nvs", 1, 2, 0x9000, 0x7000)])),
                          dict(old_table=b"invalid".ljust(4096, b"\xff")),
                          dict(old_table=table([("nvs", 1, 2, 0x9000, 0x6000), ("user", 1, 2, 0x20000, 0x1000)]))):
            with self.subTest(arguments=arguments):
                with self.assertRaises(ValueError): self.simulate(**arguments)

    def test_identify_reads_board_identity_without_writing(self):
        self.assertEqual(self.simulate(identify=True), {"chip": "ESP32-S2", "mac": "02:00:00:00:20:03"})

    def test_erase_requires_matching_mac_and_supports_foreign_layout(self):
        for mac in (None, "02:00:00:00:00:99"):
            with self.subTest(mac=mac), self.assertRaises(ValueError):
                self.simulate(erase=True, expected_mac=mac)
        self.assertTrue(self.simulate(erase=True, expected_mac="02:00:00:00:20:03",
                                      old_table=b"foreign layout")["verified"])

    def test_settings_corruption_after_write_is_reported(self):
        with self.assertRaisesRegex(RuntimeError, "Settings verification failed"):
            self.simulate(corrupt_nvs=True)

    def add_receiver_cache(self):
        entries = [(p.name, p.kind, p.subtype, p.offset, p.size) for p in
                   partitions((self.directory / 'controller/partitions.bin').read_bytes())]
        entries += [('s2_cache_0', 1, 0x40, 0x960000, 0x200000), ('s2_cache_1', 1, 0x40, 0xb60000, 0x200000)]
        data = bytearray(512); data[0] = 0xe9; data[12] = 2; data[23] = 1
        data[32:36] = bytes.fromhex('3254cdab'); data[80:100] = b'kmswitcher_receiver\0'
        data += hashlib.sha256(data).digest()
        metadata = dict(schema=1, role='receiver', chip=2, git='a' * 40, utc='2026-10-28T00:00:00Z',
            sha256=hashlib.sha256(data).hexdigest(), size=len(data), management_protocol=1, input_protocol=2, ota_schema=1)
        partition = self.document['roles']['controller']['files'][1]
        for path, address, payload in (('controller/partitions.bin', 0x8000, table(entries)),
                ('controller/cache.bin', 0x960000, image_blob(data, metadata)),
                ('controller/cache-empty.bin', 0xb60000, b'\xff' * 4096)):
            (self.directory / path).write_bytes(payload)
            record = dict(path=path, offset=address, size=len(payload), sha256=hashlib.sha256(payload).hexdigest())
            if path == partition['path']: partition.update(record)
            else: self.document['roles']['controller']['files'].append(record)
        self.save()

    def test_preloaded_cache_accepts_only_reserved_slots_and_preserves_existing_nvs(self):
        self.add_receiver_cache()
        self.assertEqual(len(Bundle(self.directory).files('controller')), 6)
        self.assertTrue(self.simulate(role='controller', chip='ESP32-S3', exponent=24,
            old_table=(self.directory / 'controller/partitions.bin').read_bytes())['verified'])
        foreign = table([('nvs', 1, 2, 0x9000, 0x6000), ('user-data', 1, 0x40, 0x960000, 0x200000)])
        with self.assertRaisesRegex(ValueError, 'persistent'):
            self.simulate(role='controller', chip='ESP32-S3', exponent=24, old_table=foreign)
        item = self.document['roles']['controller']['files'][-1]
        (self.directory / item['path']).write_bytes(b'\0' * 4096)
        item['sha256'] = hashlib.sha256(b'\0' * 4096).hexdigest(); self.save()
        with self.assertRaisesRegex(ValueError, 'empty'): Bundle(self.directory).files('controller')
