"""Locate independent desktop tools and remember portable installations."""
import json
import os
from pathlib import Path
import shutil
import sys
import webbrowser


def settings_path():
    base = os.environ.get('LOCALAPPDATA' if os.name == 'nt' else 'XDG_CONFIG_HOME', Path.home() / '.config')
    return Path(base) / 'KMSwitcher/tools.json'


def load_settings():
    try:
        value = json.loads(settings_path().read_text(encoding='utf-8'))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(values):
    path = settings_path(); path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(values, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def setup_candidates():
    name = 'kms-setup.exe' if os.name == 'nt' else 'kms-setup'
    saved = load_settings().get('setup')
    if isinstance(saved, str): yield Path(saved)
    if os.name == 'nt':
        import winreg
        key_name = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\{81988AF7-DB48-43CC-A44E-E5BD20185213}_is1'
        for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            try:
                with winreg.OpenKey(hive, key_name, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                    directory, _ = winreg.QueryValueEx(key, 'InstallLocation')
                    yield Path(directory) / name
            except OSError:
                pass
        yield Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'Programs/KMSwitcher Setup' / name
    else:
        yield Path('/opt/kmswitcher-setup') / name
    yield Path(sys.executable).parent / name
    yield Path(sys.executable).parent.parent / 'kmswitcher-setup' / name
    found = shutil.which(name)
    if found: yield Path(found)


def find_setup():
    return next((p.resolve() for p in setup_candidates() if p.is_file() and
                 (os.name == 'nt' or os.access(p, os.X_OK))), None)


def open_setup(parent, task=None, *, configure=False):
    from PySide6.QtWidgets import QFileDialog, QMessageBox
    executable = None if configure else find_setup()
    if executable is None:
        value, _ = QFileDialog.getOpenFileName(parent,
            'Locate Firmware Setup — install it separately or select its portable kms-setup launcher',
            filter='Firmware Setup (kms-setup.exe)' if os.name == 'nt' else 'Firmware Setup (kms-setup);;All files (*)')
        if not value: return False
        executable = Path(value).resolve()
    # Older Setup releases ignore the hint and still open their main page.
    started = start_detached(executable, ['ui'], KMS_SETUP_INITIAL_TASK=task or '', PYINSTALLER_RESET_ENVIRONMENT='1')
    if not started:
        QMessageBox.warning(parent, 'Firmware Setup', 'Unable to start Firmware Setup. Use Locate Firmware Setup to select its current launcher.')
        return False
    _remember(parent, setup=str(executable))
    return True


def find_browser(name):
    binary = 'msedge' if name == 'Microsoft Edge' else 'google-chrome'
    found = shutil.which(binary) or shutil.which(binary + '.exe')
    if found: return Path(found)
    if os.name == 'nt':
        suffix = 'Microsoft/Edge/Application/msedge.exe' if name == 'Microsoft Edge' else 'Google/Chrome/Application/chrome.exe'
        for variable in ('PROGRAMFILES', 'PROGRAMFILES(X86)', 'LOCALAPPDATA'):
            root = os.environ.get(variable)
            if root and (Path(root) / suffix).is_file(): return Path(root) / suffix
    elif name == 'Google Chrome':
        found = shutil.which('chromium') or shutil.which('chromium-browser')
        if found: return Path(found)
    return None


def _remember(parent, **settings):
    from PySide6.QtWidgets import QMessageBox
    try:
        values = load_settings(); values.update(settings); save_settings(values)
    except OSError as error:
        QMessageBox.warning(parent, 'Tool location', f'The tool opened, but its location could not be saved: {error}')


def start_detached(executable, arguments, **settings):
    from PySide6.QtCore import QProcess, QProcessEnvironment
    environment = QProcessEnvironment.systemEnvironment()
    if getattr(sys, 'frozen', False) and os.name != 'nt':
        if environment.contains('LD_LIBRARY_PATH_ORIG'):
            environment.insert('LD_LIBRARY_PATH', environment.value('LD_LIBRARY_PATH_ORIG'))
        else:
            environment.remove('LD_LIBRARY_PATH')
    for key, value in settings.items(): environment.insert(key, value)
    process = QProcess()
    process.setProgram(str(executable)); process.setArguments(arguments)
    process.setWorkingDirectory(str(executable.parent)); process.setProcessEnvironment(environment)
    result = process.startDetached()
    return result[0] if isinstance(result, tuple) else result


def open_console(parent, *, configure=False):
    from PySide6.QtWidgets import QFileDialog, QInputDialog, QMessageBox
    values = load_settings()
    saved = values.get('console')
    path = Path(saved) if isinstance(saved, str) else Path()
    browser = values.get('browser', 'Default browser')
    if configure or not path.is_file():
        value, _ = QFileDialog.getOpenFileName(parent, 'Locate Engineering Console', filter='KMSwitcher console (*.html)')
        if not value: return False
        path = Path(value).resolve()
        browser, accepted = QInputDialog.getItem(parent, 'Console browser',
            'Device access requires Web Serial. Choose Chrome or Edge if your default browser does not support it.',
            ['Default browser', 'Microsoft Edge', 'Google Chrome'], 0, False)
        if not accepted: return False
    uri = path.resolve().as_uri()
    if browser == 'Default browser':
        opener = shutil.which('xdg-open') if os.name != 'nt' else None
        started = start_detached(Path(opener), [uri]) if opener else webbrowser.open(uri)
    else:
        executable = find_browser(browser)
        if executable is None:
            QMessageBox.warning(parent, 'Console browser', f'{browser} was not found. Use Locate Engineering Console to select another browser.')
            return False
        started = start_detached(executable, [uri])
    if not started:
        QMessageBox.warning(parent, 'Engineering Console', 'Unable to open the console in the selected browser.')
        return False
    _remember(parent, console=str(path.resolve()), browser=browser)
    return True
