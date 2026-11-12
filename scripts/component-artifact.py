"""Publish immutable component metadata and firmware ZIPs from validated outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'companion'))


def record(directory, identity):
    directory = Path(directory)
    files = [dict(path=p.relative_to(directory).as_posix(), size=p.stat().st_size,
                  sha256=hashlib.sha256(p.read_bytes()).hexdigest())
             for p in sorted(directory.rglob('*')) if p.is_file() and p.name != 'component.json']
    if not files:
        raise ValueError('Cannot publish an empty component')
    (directory / 'component.json').write_text(json.dumps(dict(identity, files=files), indent=2) + '\n')


def firmware_zip(directory, identity, product="regular"):
    from kms_update.firmware import Bundle
    directory = Path(directory)
    bundle = Bundle(directory, product=product).validate()
    if any(bundle[key] != identity[key] for key in ('commit', 'version')):
        raise ValueError('Firmware identity mismatch')
    # Archive before adding publication metadata, so every consumer sees exactly
    # one bundle and the archive never includes itself or prior archives.
    for old in directory.glob('*.zip'):
        raise ValueError('Firmware staging already contains an archive: ' + str(old))
    archive = directory.parent / ('KMSwitcher-' + product + '-firmware-' + identity['version'])
    path = Path(shutil.make_archive(str(archive), 'zip', directory))
    shutil.move(str(path), directory / path.name)
    archive = directory / path.name
    index = dict(schema=1, releases=[dict(product=product, version=identity['version'], commit=identity['commit'],
        stable=identity['stable'], url=archive.name, size=archive.stat().st_size,
        sha256=hashlib.sha256(archive.read_bytes()).hexdigest())])
    (directory / 'release-index.json').write_text(json.dumps(index, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--identity', type=Path, default=ROOT / 'build/release/version.json')
    parser.add_argument('--firmware', action='store_true')
    parser.add_argument('--stress-firmware', action='store_true')
    args = parser.parse_args()
    identity = json.loads(args.identity.read_text())
    if args.firmware or args.stress_firmware:
        firmware_zip(args.directory, identity, "stress" if args.stress_firmware else "regular")
    record(args.directory, identity)
