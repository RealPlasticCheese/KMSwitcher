"""Create a verified, settings-preserving wired/OTA firmware bundle from IDF outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "companion"))
from kms_update.firmware import Bundle, ROLES
from kms_update.cache import bundled_receiver, validate_blob


def package(build, output, identity, allow_dirty=False):
    document = dict(schema=1, version=identity["version"], commit=identity["commit"], roles={})
    for role, (chip, _, capacity) in ROLES.items():
        source = build / role
        manifest = json.loads((source / f"kmswitcher_{role}.json").read_text())
        expected = identity["commit"]
        if manifest["git"] != expected and not (allow_dirty and manifest["git"] == expected + "-dirty"):
            raise ValueError(f"{role}: firmware was built from another source revision")
        if manifest["role"] != role or manifest["input_protocol"] != 2 or manifest["management_protocol"] != 1:
            raise ValueError("Firmware protocol/role mismatch")
        if role == 'controller':
            receiver = bundled_receiver(manifest)
            if not receiver or receiver[1]['git'] != manifest['git']:
                raise ValueError('Controller release must bundle its matching receiver firmware')
            cached = validate_blob((source / 'receiver-cache.bin').read_bytes())
            if cached['sha256'] != receiver[1]['sha256']:
                raise ValueError('Preloaded cache and controller OTA payload differ')
        config = json.loads((source / "config/sdkconfig.json").read_text())
        if config.get("IDF_TARGET") != chip or not config.get("BOOTLOADER_APP_ROLLBACK_ENABLE") or not config.get("PARTITION_TABLE_CUSTOM"):
            raise ValueError("Release requires the matching chip, OTA partition table and rollback support")
        if not config.get("KMS_TEST_SUPPORT"):
            raise ValueError("Distribution firmware must retain existing recovery controls; rebuild with -Release -ResetConfig")
        if role == "receiver" and config.get("TINYUSB_HID_COUNT") != 3:
            raise ValueError("Receiver release requires all three HID interfaces")
        flash = json.loads((source / "flasher_args.json").read_text())
        if flash["extra_esptool_args"]["chip"] != chip:
            raise ValueError("Flash metadata targets a different chip")
        entries = []
        for offset, filename in flash["flash_files"].items():
            path = (source / filename).resolve()
            if not path.is_relative_to(source.resolve()):
                raise ValueError("Flash image path escapes the build")
            destination = output / role / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            data = destination.read_bytes()
            entries.append(dict(path=destination.relative_to(output).as_posix(), offset=int(offset, 0),
                                size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        app = next(item for item in entries if item["path"].endswith(f"kmswitcher_{role}.bin"))
        if (app["size"], app["sha256"]) != (manifest["size"], manifest["sha256"]):
            raise ValueError("Application and OTA manifest do not match")
        shutil.copy2(source / f"kmswitcher_{role}.json", output / role)
        # Keep the actual IDF offsets and SDK settings for recovery and audit.
        shutil.copy2(source / "flasher_args.json", output / role)
        for name in ("project_description.json", "size.txt", "size.json"):
            if (source / name).exists():
                shutil.copy2(source / name, output / role)
        document["roles"][role] = dict(chip=chip, capacity=capacity, settings=flash["flash_settings"], files=entries)
    (output / "bundle.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    Bundle(output).validate()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/firmware")
    parser.add_argument("--identity", type=Path, default=ROOT / "build/release/version.json")
    parser.add_argument("--allow-dirty", action="store_true", help="Local development only")
    args = parser.parse_args()
    if args.allow_dirty and __import__("os").environ.get("TF_BUILD"):
        raise SystemExit("Dirty firmware is prohibited in Azure builds")
    package(args.build, args.output, json.loads(args.identity.read_text()), args.allow_dirty)
    print(f"Validated firmware bundle: {args.output}")
