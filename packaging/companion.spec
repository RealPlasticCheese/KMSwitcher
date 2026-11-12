# Two launchers share a replaceable on-disk runtime, including Qt shared libraries.
import os
import sys
from pathlib import Path
from PyInstaller.utils.hooks import collect_data_files, collect_submodules

source = Path(os.environ['KMS_PACKAGE_SOURCE'])
sys.path.insert(0, str(source))
product = os.environ.get('KMS_PACKAGE_PRODUCT', 'companion')
hidden = [] if product == 'companion' else collect_submodules('esptool') + collect_submodules('kms_update')
data = [] if product == 'companion' else collect_data_files('esptool')
if product == 'companion':
    hidden += ['hidraw' if sys.platform == 'linux' else 'hid', 'kms_companion.ui']
    data += [(str(source / 'kms_companion/VERSION'), 'kms_companion'),
             (str(source / 'kms_companion/build-info.json'), 'kms_companion')]
if product == 'stress':
    hidden += ['evdev', 'pyudev', 'tkinter']
    data += [(str(source / 'kms_stress/web_assets'), 'kms_stress/web_assets')]
    data += [(str(source / 'kms_stress/build-info.json'), 'kms_stress')]

def analyze(entry):
    return Analysis([str(source / entry)], pathex=[str(source)], binaries=[], datas=data,
        hiddenimports=hidden, hookspath=[], excludes=['PyQt5', 'PyQt6', 'PySide2'] + ([] if product == 'stress' else ['kms_stress']) + (['kms_update', 'esptool'] if product == 'companion' else ['kms_companion']), noarchive=False)

entries = [] if product == 'companion' else [('firmware-entry.py', 'kms-firmware', True), ('setup-entry.py', 'kms-setup', False)]
if product == 'companion': entries += [('cli-entry.py', 'kmswitcher', True), ('gui-entry.py', 'kmswitcher-gui', False)]
if product == 'stress': entries += [('stress-entry.py', 'kms-stress', True)]
analyses, programs = [], []
for entry, name, console in entries:
    analysis = analyze(entry)
    analyses += [analysis.binaries, analysis.datas]
    programs.append(EXE(PYZ(analysis.pure), analysis.scripts, [], exclude_binaries=True, name=name,
        console=console, upx=False, version=os.environ.get('KMS_WINDOWS_VERSION_FILE')))
COLLECT(*programs, *analyses, strip=False, upx=False,
        name={'companion': 'kmswitcher', 'setup': 'kmswitcher-setup', 'stress': 'kmswitcher-stress'}[product])
