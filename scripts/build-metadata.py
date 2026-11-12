"""Generate automatic source identity and an exact-binary firmware manifest."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess


def git_identity(root):
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    changed = subprocess.check_output(
        ['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=normal'], text=True)
    return revision + ('-dirty' if changed else '')


def stamp(root, header):
    identity = {'git': git_identity(root),
                'utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('#pragma once\n' + ''.join(
        f'#define KMS_BUILD_{key.upper()} {json.dumps(value)}\n' for key, value in identity.items()),
        encoding='utf-8')
    header.with_suffix('.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')


def manifest(binary, header, role, chip):
    data = binary.read_bytes()
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError('Not an ESP application image')
    actual_chip = int.from_bytes(data[12:14], 'little')
    if actual_chip != chip:
        raise ValueError('Firmware chip does not match the build target')
    identity = json.loads(header.with_suffix('.json').read_text(encoding='utf-8'))
    if any(identity[field].encode('ascii') + b'\0' not in data for field in ('git', 'utc')):
        raise ValueError('Generated build identity does not match the application binary')
    if role in ('receiver', 'controller'):
        config = json.loads((binary.parent / 'config/sdkconfig.json').read_text(encoding='utf-8'))
        if not config.get('BOOTLOADER_APP_ROLLBACK_ENABLE') or not config.get('PARTITION_TABLE_CUSTOM'):
            raise ValueError(f'{role.title()} requires OTA partitions and rollback; rebuild with -ResetConfig')
    identity.update(schema=1, role=role, chip=chip, image=binary.name,
                    size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                    management_protocol=1, input_protocol=2, ota_schema=1)
    binary.with_suffix('.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('stamp')
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--header', type=Path, required=True)
    p = sub.add_parser('manifest')
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--header', type=Path, required=True)
    p.add_argument('--role', choices=['controller', 'receiver'], required=True)
    p.add_argument('--chip', type=int, required=True)
    args = parser.parse_args()
    if args.action == 'stamp':
        stamp(args.root, args.header)
    else:
        manifest(args.binary, args.header, args.role, args.chip)


if __name__ == '__main__':
    main()
