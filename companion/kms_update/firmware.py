"""Validate release bundles and flash supported boards without erasing pairing NVS."""
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from .cache import OFFSETS as CACHE_OFFSETS, SLOT_BYTES as CACHE_SLOT_BYTES, validate_blob

ROLES = {"controller": ("esp32s3", 9, 16 * 1024 * 1024),
         "receiver": ("esp32s2", 2, 4 * 1024 * 1024)}
GENERATOR = ("esp32s3", 9, 16 * 1024 * 1024)


@dataclass(frozen=True)
class Partition:
    name: str
    kind: int
    subtype: int
    offset: int
    size: int


def partitions(data):
    """Read ESP-IDF partition entries, including the optional table MD5 record."""
    entries = []
    for offset in range(0, len(data) - 31, 32):
        entry = data[offset:offset + 32]
        if entry == b"\xff" * 32:
            break
        if entry[:2] == b"\xeb\xeb":
            if hashlib.md5(data[:offset]).digest() != entry[16:]:
                raise ValueError("Partition table checksum failed")
            break
        magic, kind, subtype, address, size, label, flags = struct.unpack("<HBBII16sI", entry)
        if magic != 0x50AA or not size or flags:
            raise ValueError("Unsupported or corrupt partition table")
        entries.append(Partition(label.rstrip(b"\0").decode("ascii"), kind, subtype, address, size))
    if not entries:
        raise ValueError("No supported partition table found")
    return entries


def overlaps(start, size, partition):
    # Flash writes erase whole 4 KiB sectors, including a partially used final sector.
    return start < partition.offset + partition.size and (start + size + 4095) // 4096 * 4096 > partition.offset


def receiver_cache_partition(partition):
    return (partition.kind == 1 and partition.subtype == 0x40 and partition.size == CACHE_SLOT_BYTES and
            (partition.name, partition.offset) in (('s2_cache_0', CACHE_OFFSETS[0]), ('s2_cache_1', CACHE_OFFSETS[1])))


def default_bundle():
    if getattr(sys, "frozen", False):
        return Path(sys.executable).parent / "firmware"
    return Path(__file__).resolve().parents[2] / "build/release/firmware"


class Bundle:
    def __init__(self, directory=None, *, product=None):
        self.directory = Path(directory) if directory else default_bundle()
        self.document = json.loads((self.directory / "bundle.json").read_text(encoding="utf-8"))
        self.product = self.document.get("product", "regular")
        expected = set(ROLES) | ({"generator"} if self.product == "stress" else set())
        if (self.document.get("schema") != 1 or self.product not in ("regular", "stress") or
                set(self.document.get("roles", {})) != expected or (product and product != self.product)):
            raise ValueError("Unsupported firmware bundle")

    def files(self, role):
        chip, chip_id, capacity = GENERATOR if role == "generator" and self.product == "stress" else ROLES[role]
        description = self.document["roles"][role]
        if description["chip"] != chip or description["capacity"] != capacity:
            raise ValueError("Firmware targets a different board")
        result = []
        for item in description["files"]:
            path = (self.directory / item["path"]).resolve()
            if not path.is_relative_to(self.directory.resolve()):
                raise ValueError("Firmware filename escapes the bundle")
            data = path.read_bytes()
            if len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError(f"Firmware checksum failed: {path.name}")
            address = item["offset"]
            if type(address) is not int or address < 0 or address % 4096 or not data or address + len(data) > capacity:
                raise ValueError("Invalid firmware flash range")
            result.append((address, data))
        has_cache = role == 'controller' and any(address in CACHE_OFFSETS for address, _ in result)
        count = 3 if role == "generator" else 6 if has_cache else 4
        if len(result) != count or len({address for address, _ in result}) != count:
            raise ValueError("Expected bootloader, partition table, OTA data and application")
        table = next((data for address, data in result if address == 0x8000), None)
        if table is None or len(table) > 4096:
            raise ValueError("Missing partition table")
        layout = partitions(table)
        nvs = next((p for p in layout if p.name == "nvs" and p.kind == 1 and p.subtype == 2), None)
        if nvs is None or (nvs.offset, nvs.size) != (0x9000, 0x6000):
            raise ValueError("This layout cannot preserve KMSwitcher pairing and settings")
        app = next((p for p in layout if p.kind == 0 and p.subtype == (0x10 if role == "receiver" else 0)), None)
        ota = next((p for p in layout if p.kind == 1 and p.subtype == 0), None)
        boot = 0x1000 if role == "receiver" else 0
        expected = {boot, 0x8000, app.offset} if app else set()
        if role != "generator" and ota:
            expected.add(ota.offset)
        if has_cache:
            expected.update(CACHE_OFFSETS)
            if len([p for p in layout if receiver_cache_partition(p)]) != 2:
                raise ValueError('Receiver cache requires its dedicated two-slot layout')
        if app is None or (role != "generator" and ota is None) or {a for a, _ in result} != expected:
            raise ValueError("Unsupported initial-install layout")
        for address, data in result:
            if has_cache and address in CACHE_OFFSETS:
                if address == CACHE_OFFSETS[0]:
                    validate_blob(data)
                elif data != b'\xff' * 4096:
                    raise ValueError('Second receiver-cache header must be empty on installation')
                continue
            if address in (boot, app.offset):
                if len(data) < 24 or data[0] != 0xE9 or int.from_bytes(data[12:14], "little") != chip_id:
                    raise ValueError("Firmware image chip does not match the selected board")
            if address == boot and address + len(data) > 0x8000:
                raise ValueError("Bootloader overlaps the partition table")
            if address == app.offset and len(data) > app.size:
                raise ValueError("Application exceeds its partition")
            if ota and address == ota.offset and len(data) > ota.size:
                raise ValueError("OTA data exceeds its partition")
            if any(overlaps(address, len(data), p) for p in layout if p.kind == 1 and p.subtype != 0):
                raise ValueError("Firmware would overwrite persistent device data")
        for index, (address, data) in enumerate(result):
            if any(address < other + len(payload) and other < address + len(data)
                   for other, payload in result[index + 1:]):
                raise ValueError('Overlapping firmware images')
        return result

    def validate(self):
        for role in self.document["roles"]:
            self.files(role)
        return {"version": self.document["version"], "commit": self.document["commit"],
                "product": self.product, "roles": list(self.document["roles"]), "directory": str(self.directory.resolve())}

    def application(self, role):
        """Return the verified OTA application; generated metadata is checked separately."""
        self.files(role)
        path = self.directory / role / f"kmswitcher_{role}.bin"
        relative = path.relative_to(self.directory).as_posix()
        if not any(item["path"] == relative for item in self.document["roles"][role]["files"]):
            raise ValueError("OTA application is not included in the verified bundle")
        from .management import load_firmware
        image, metadata = load_firmware(path, role)
        if metadata.get("git") not in (self.document["commit"], self.document["commit"] + "-dirty"):
            raise ValueError("OTA application comes from another release")
        return image, metadata


def ports():
    from serial.tools import list_ports
    return [dict(port=p.device, description=p.description, serial=p.serial_number,
                 vid=p.vid, pid=p.pid) for p in list_ports.comports()]


def flash(role, port, directory=None, *, expected_mac=None, backup_directory=None, identify=False, erase=False):
    from .fixture_lock import fixture_lock
    with fixture_lock():
        return _flash(role, port, directory, expected_mac=expected_mac, backup_directory=backup_directory,
                      identify=identify, erase=erase)


def _flash(role, port, directory=None, *, expected_mac=None, backup_directory=None, identify=False, erase=False):
    """Explicitly selected port only; reject chip/layout mismatches before writing."""
    from esptool.cmds import attach_flash, detect_chip, erase_flash, read_flash, reset_chip, run_stub, write_flash, verify_flash
    from . import config_directory

    if erase and (identify or not expected_mac):
        raise ValueError("Erase & Flash requires the confirmed board MAC. Nothing was written.")
    bundle = Bundle(directory)
    files = bundle.files(role)  # Validate every byte before opening a port.
    chip, _, capacity = GENERATOR if role == "generator" and bundle.product == "stress" else ROLES[role]
    mode = "no-reset" if role == "receiver" else "default-reset"
    with detect_chip(port, connect_mode=mode, connect_attempts=3) as connection:
        actual_chip = connection.CHIP_NAME.lower().replace("-", "")
        if actual_chip != chip:
            raise ValueError(f"Selected {role} needs {chip}; connected board is {connection.CHIP_NAME}. Nothing was written.")
        mac = ":".join(f"{n:02x}" for n in connection.read_mac())
        if expected_mac and expected_mac.lower() != mac:
            raise ValueError("Connected board differs from the expected MAC. Nothing was written.")
        if identify:
            return {"chip": connection.CHIP_NAME, "mac": mac}
        esp = run_stub(connection)
        attach_flash(esp)
        exponent = (esp.flash_id() >> 16) & 255
        if not 18 <= exponent <= 30 or (1 << exponent) < capacity:
            raise ValueError("Board flash capacity is smaller than this firmware requires. Nothing was written.")
        filename = None
        if erase:
            erase_flash(esp)
        else:
            old_table = read_flash(esp, 0x8000, 4096)
            if old_table != b"\xff" * len(old_table):
                old_layout = partitions(old_table)
                nvs = next((p for p in old_layout if p.name == "nvs" and p.kind == 1 and p.subtype == 2), None)
                if nvs is None or (nvs.offset, nvs.size) != (0x9000, 0x6000):
                    raise ValueError("Existing pairing layout differs. Nothing was written; use the recovery guide.")
                for address, data in files:
                    if any(overlaps(address, len(data), p) for p in old_layout if p.kind == 1 and p.subtype != 0 and
                           not (role == 'controller' and address in CACHE_OFFSETS and receiver_cache_partition(p))):
                        raise ValueError("Update overlaps existing persistent data. Nothing was written.")
            backup = read_flash(esp, 0x9000, 0x6000)
            folder = Path(backup_directory) if backup_directory else config_directory() / "firmware-backups"
            folder.mkdir(parents=True, exist_ok=True, mode=0o700)
            prefix = mac.replace(":", "") + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-"
            descriptor, filename = tempfile.mkstemp(prefix=prefix, suffix=".nvs", dir=folder)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(backup)
            print(f"Device: {chip} {mac}. Pairing/settings backup: {filename}", flush=True)
        settings = bundle.document["roles"][role]["settings"]
        # esptool verifies the written MD5 before returning.
        write_flash(esp, files, flash_mode=settings["flash_mode"], flash_freq=settings["flash_freq"],
                    flash_size=settings["flash_size"])
        verify_flash(esp, files, flash_mode=settings["flash_mode"], flash_freq=settings["flash_freq"],
                     flash_size=settings["flash_size"])
        if not erase and read_flash(esp, 0x9000, 0x6000) != backup:
            raise RuntimeError(f"Settings verification failed; backup is at {filename}")
        if role == "controller":
            reset_chip(esp, "hard-reset")
    return {"role": role, "mac": mac, "version": bundle.document["version"], "verified": True,
            "backup": str(filename) if filename else None, "next": "Unplug and reconnect normally, without holding BOOT."}
