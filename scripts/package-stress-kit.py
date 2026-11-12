"""Package the Linux app and everyday/test firmware, without flashing."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
version = runpy.run_path(str(root / 'linux/kms_stress/__init__.py'))['__version__']
parser = argparse.ArgumentParser(description=__doc__)
selection = parser.add_mutually_exclusive_group()
selection.add_argument('--app-only', action='store_true', help='Package a Linux app update without including firmware')
selection.add_argument('--usb-isolation', action='store_true', help='Package the minimal spare-master firmware and Linux runner')
parser.add_argument('--generator-trace', action='store_true', help='Include both traced generator flash images with --usb-isolation')
args = parser.parse_args()
if args.generator_trace and not args.usb_isolation: parser.error('--generator-trace requires --usb-isolation')
destination = root / ('build/KMSwitcher-usb-isolation.zip' if args.usb_isolation else
                      'build/KMSwitcher-linux-app.zip' if args.app_only else 'build/KMSwitcher-stress-kit.zip')
destination.parent.mkdir(parents=True, exist_ok=True)
firmware = (
    ('hid-generator-esp32s3', 'kmswitcher_hid_generator.bin', 'generator'),
    ('controller', 'kmswitcher_controller.bin', 'controller'),
    ('receiver', 'kmswitcher_receiver.bin', 'receiver'),
) if not (args.app_only or args.usb_isolation) else ()
inputs, manifests = [], []
for directory, binary, role in firmware:
    path = root / 'build' / directory / binary
    if not path.is_file(): raise SystemExit(f'Missing {path}; run scripts/build-stress-kit.ps1 first')
    data = path.read_bytes()
    manifests.append({'role': role, 'file': binary, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    inputs.append((path, 'firmware/' + binary))
    sidecar = path.with_suffix('.json')
    if role != 'generator':
        metadata = json.loads(sidecar.read_text())
        if metadata['sha256'] != manifests[-1]['sha256'] or metadata['role'] != role:
            raise SystemExit(f'Stale/mismatched firmware manifest: {sidecar}')
        config = json.loads((path.parent / 'config/sdkconfig.json').read_text())
        if not config.get('KMS_TEST_SUPPORT'): raise SystemExit(f'{role} image has no test support')
        inputs.append((sidecar, 'firmware/' + sidecar.name))
linux_root = root / 'linux'
for path in (root / 'companion/kms_update').rglob('*.py'):
    inputs.append((path, 'companion/kms_update/' + path.name))
for path in linux_root.rglob('*'):
    relative = path.relative_to(linux_root)
    if path.is_file() and not set(relative.parts) & {'.venv', '__pycache__', 'build'} and not any(p.endswith('.egg-info') for p in relative.parts):
        inputs.append((path, 'linux/' + relative.as_posix()))
inputs.append((root / 'docs/stress-testing.md', 'START-HERE.md'))
inputs.append((root / 'docs/usb-isolation.md', 'usb-isolation.md'))
if args.usb_isolation:
    build = root / 'build/usb-isolation-esp32s3'
    project = json.loads((build / 'project_description.json').read_text())
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    if project['project_version'] != revision[:12]: raise SystemExit('Rebuild isolation firmware at the current commit before packaging')
    excluded = {'kms_core', 'kms_runtime', 'esp_wifi', 'espressif__usb_host_hid', 'espressif__tinyusb'}
    if excluded & set(project['build_components']): raise SystemExit('Isolation image includes an excluded application/device component')
    if 'espressif__usb' not in project['build_components']: raise SystemExit('Isolation image is missing USB host support')
    for source, target, offset in (
        ('kmswitcher_usb_isolation.bin', 'kmswitcher_usb_isolation.bin', 0x10000),
        ('bootloader/bootloader.bin', 'usb-isolation-bootloader.bin', 0),
        ('partition_table/partition-table.bin', 'usb-isolation-partitions.bin', 0x8000)):
        path = build / source; data = path.read_bytes()
        inputs.append((path, 'firmware/' + target))
        manifests.append({'role': 'usb-isolation', 'file': target, 'offset': hex(offset), 'bytes': len(data),
                          'sha256': hashlib.sha256(data).hexdigest(), 'source_revision': revision})
    for path in (root / 'diagnostics/usb-isolation').rglob('*'):
        if path.is_file() and not set(path.relative_to(root).parts) & {'managed_components', 'build', '__pycache__'}:
            inputs.append((path, 'reproducer/' + path.relative_to(root).as_posix()))
    for path in (root / 'firmware/usb_frame_guard').rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts:
            inputs.append((path, 'reproducer/' + path.relative_to(root).as_posix()))
    for name in ('diagnostics/hid-generator/main/pattern.c', 'diagnostics/hid-generator/main/pattern.h',
                 'config/sdkconfig.flash.esp32s3', 'config/sdkconfig.usb-isolation.esp32s3', 'config/partitions.usb-isolation.csv'):
        inputs.append((root / name, 'reproducer/' + name))
    inputs.append((root / 'docs/usb-isolation.md', 'USB-ISOLATION.md'))
    inputs.append((root / 'docs/usb-driver-trace.md', 'DRIVER-TRACE.md'))
    inputs.append((root / 'docs/usb-driver-trace-2026-10-23.md', 'usb-driver-trace-2026-10-23.md'))
    if args.generator_trace:
        generator_build = root / 'build/hid-generator-esp32s3'
        generator_project = json.loads((generator_build / 'project_description.json').read_text())
        if generator_project['project_version'] != revision[:12]:
            raise SystemExit('Rebuild generator firmware at the current commit before packaging')
        for directory, option in [(build, 'KMS_ISO_DRIVER_TRACE'), (generator_build, 'KMS_GEN_DRIVER_TRACE')]:
            if f'{option}:BOOL=ON' not in (directory / 'CMakeCache.txt').read_text():
                raise SystemExit(f'{option} must be ON for the combined trace kit')
        for source, target, offset in (
            ('kmswitcher_hid_generator.bin', 'kmswitcher_hid_generator.bin', 0x10000),
            ('bootloader/bootloader.bin', 'generator-bootloader.bin', 0),
            ('partition_table/partition-table.bin', 'generator-partitions.bin', 0x8000)):
            path = generator_build / source; data = path.read_bytes()
            inputs.append((path, 'firmware/' + target))
            manifests.append({'role': 'generator', 'file': target, 'offset': hex(offset), 'bytes': len(data),
                              'sha256': hashlib.sha256(data).hexdigest(), 'source_revision': revision})
        for path in (root / 'diagnostics/hid-generator').rglob('*'):
            if (path.is_file() and not set(path.relative_to(root).parts) & {'managed_components', 'build', '__pycache__'}
                    and path.name not in {'pattern.c', 'pattern.h'}):
                inputs.append((path, 'reproducer/' + path.relative_to(root).as_posix()))
with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    for path, name in inputs:
        entry = zipfile.ZipInfo(name)
        entry.create_system = 3
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = (0o100755 if path.suffix == '.sh' else 0o100644) << 16
        archive.writestr(entry, path.read_bytes())
    if manifests:
        archive.writestr('firmware/SHA256.json', json.dumps(manifests, indent=2) + '\n')
    if args.app_only:
        archive.writestr('UPDATE-LINUX.txt',
            'Close KMSwitcher Test on Linux. Extract this archive over the original kit folder\n'
            '(the folder containing linux/, not inside linux/). Reopen the same app-menu entry\n'
            f'or run bash run.sh from linux/. The title should show version {version}.\n'
            'For the browser dashboard, run bash runweb.sh and open the access URL printed\n'
            'in the terminal. Saved reports can be viewed or downloaded as complete ZIP files.\n'
            'Saved profiles remain in your home directory. This app requires master firmware\n'
            'with the test-mode command. Install the matching everyday/test master and receiver\n'
            'firmware from the full kit before running a test. The desktop dashboard remains\n'
            'available. USB-stall campaigns additionally require updated S3 firmware with\n'
            'the usb-status <ID> command. Existing S2 firmware is compatible. Update both\n'
            'tester S3s with the accompanying generator firmware to fix pending transfers\n'
            'after a USB host reset. Older generators remain protocol-compatible, but\n'
            'the app will stop the campaign if they cannot complete neutral recovery.\n'
            f'App {version} accepts USB Host 1.5.0/1.6.0 and traced host/generator identities.\n'
            'USB isolation defaults to --workaround true and requires the new spare-master image;\n'
            'pass --workaround false for older images or a matched control run.\n'
            'Generator firmware 75af11561422 remains compatible with this experiment.\n'
            'The combined USB trace kit updates the spare host and both generators.\n'
            'See usb-isolation.md for the separate spare-master USB-only experiment.\n'
            'No new Python dependencies are required for the browser dashboard.\n')
print(destination)
