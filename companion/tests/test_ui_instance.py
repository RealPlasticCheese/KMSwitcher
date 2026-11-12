"""Exercise repeated launches and shutdown across real Qt processes."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


UI_PROCESS = """
import os
from pathlib import Path
import time
from unittest.mock import patch
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication
from kms_companion.ui import launch

directory = Path(os.environ['LOCALAPPDATA'])
while not (directory / 'start').exists():
    time.sleep(.01)
app = QApplication([])
def helper():
    with (directory / 'helper-starts').open('a') as stream:
        stream.write('started\\n')
QTimer.singleShot(20000, app.quit)
with patch('kms_companion.cli.ensure_helper', side_effect=helper), \\
     patch('kms_companion.cli.startup_enabled', return_value=False), \\
     patch('kms_companion.ipc.call', side_effect=RuntimeError('Offline test')):
    launch(tray_only=True)
"""


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "Optional UI dependencies not installed")
class UiInstanceTests(unittest.TestCase):
    def wait_until(self, predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.05)
        self.fail("Timed out waiting for UI processes")

    def environment(self, directory):
        return dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory,
                    QT_QPA_PLATFORM="offscreen")

    def stop_processes(self, processes):
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def test_simultaneous_launches_start_one_ui_and_one_helper_then_quit(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = self.environment(directory)
            processes = [subprocess.Popen([sys.executable, "-c", UI_PROCESS], env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(4)]
            try:
                Path(directory, "start").touch()
                self.wait_until(lambda: sum(p.poll() is not None for p in processes) == 3)
                for process in processes:
                    if process.poll() is not None:
                        self.assertEqual(process.returncode, 0, process.communicate()[1].decode())
                self.assertEqual(Path(directory, "helper-starts").read_text().splitlines(), ["started"])
                result = subprocess.run([sys.executable, "-c",
                    "from unittest.mock import patch; from kms_companion.cli import main; "
                    "\nwith patch('kms_companion.ipc.call', side_effect=RuntimeError('Offline test')): "
                    "raise SystemExit(main(['quit']))"], env=environment, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.wait_until(lambda: all(p.poll() is not None for p in processes))
                self.assertTrue(all(p.returncode == 0 for p in processes))
                self.assertFalse(Path(directory, "KMSwitcher/ui.lock").exists())
            finally:
                self.stop_processes(processes)

    def test_crashed_owner_does_not_prevent_relaunch(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = self.environment(directory)
            Path(directory, "start").touch()
            processes = []
            try:
                for count in (1, 2):
                    process = subprocess.Popen([sys.executable, "-c", UI_PROCESS], env=environment,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    processes.append(process)
                    marker = Path(directory, "helper-starts")
                    self.wait_until(lambda: marker.exists() and len(marker.read_text().splitlines()) == count)
                    self.assertIsNone(process.poll())
                    process.kill()
                    process.communicate(timeout=5)
            finally:
                self.stop_processes(processes)

    def test_declined_quit_keeps_helper_running(self):
        from kms_companion.cli import execute, parser
        with patch("kms_companion.ui_instance.send_command", return_value=True), \
             patch("kms_companion.cli.time.monotonic", side_effect=[0, 11]), \
             patch("kms_companion.ipc.call") as call:
            with self.assertRaisesRegex(RuntimeError, "still open"):
                execute(parser().parse_args(["quit"]))
            call.assert_not_called()

    def test_packaged_gui_starts_console_helper(self):
        from kms_companion.cli import ensure_helper
        with patch.object(sys, "frozen", True, create=True), \
             patch.object(sys, "executable", str(Path("app/kmswitcher-gui.exe"))), \
             patch("kms_companion.ipc.call", side_effect=[RuntimeError("Offline"), {}]), \
             patch("kms_companion.cli.subprocess.Popen") as popen, \
             patch("kms_companion.cli.time.sleep"):
            ensure_helper()
        suffix = "kmswitcher.exe" if os.name == "nt" else "kmswitcher"
        self.assertEqual(popen.call_args.args[0], [str(Path("app") / suffix), "run"])
