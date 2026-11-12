"""Extend the regular verified bundle with a complete, same-commit generator image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "companion"))
from kms_update.firmware import Bundle, GENERATOR


def package(production, generator, output):
    base = Bundle(production, product="regular"); base.validate()
    description = json.loads((generator / "project_description.json").read_text())
    if description["project_version"] != base.document["commit"][:12]:
        raise ValueError("Generator and regular firmware must be built from the same commit")
    config = json.loads((generator / "config/sdkconfig.json").read_text())
    if config["IDF_TARGET"] != "esp32s3": raise ValueError("Generator requires an ESP32-S3")
    # The regular artifact is already published when diagnostic packaging runs.
    # Copy firmware inputs only; the diagnostic artifact gets its own ZIP/index.
    shutil.copytree(production, output, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('*.zip', 'component.json', 'release-index.json'))
    document = base.document
    document["product"] = "stress"
    flash = json.loads((generator / "flasher_args.json").read_text())
    entries = []
    for offset, filename in flash["flash_files"].items():
        source = (generator / filename).resolve()
        if not source.is_relative_to(generator.resolve()): raise ValueError("Generator path escapes build")
        destination = output / "generator" / filename
        destination.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, destination)
        data = source.read_bytes()
        entries.append(dict(path=destination.relative_to(output).as_posix(), offset=int(offset, 0),
                            size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    document["roles"]["generator"] = dict(chip=GENERATOR[0], capacity=GENERATOR[2], settings=flash["flash_settings"], files=entries)
    (output / "bundle.json").write_text(json.dumps(document, indent=2) + "\n")
    Bundle(output, product="stress").validate()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--production", type=Path, default=ROOT / "build/release/firmware")
    parser.add_argument("--generator", type=Path, default=ROOT / "build/hid-generator-esp32s3")
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/stress-firmware")
    args = parser.parse_args()
    package(args.production, args.generator, args.output)
