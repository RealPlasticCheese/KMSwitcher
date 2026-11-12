"""Exercise exception capture in fresh interpreters and bounded tray recovery."""
import os
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock
from kms_companion.ui_supervisor import run


class SupervisorTests(unittest.TestCase):
    def test_abrupt_child_exit_is_recovered_in_a_real_process(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / 'starts'
            code = """
import os, sys
from pathlib import Path
marker = Path(sys.argv[1])
previous = marker.read_text() if marker.exists() else ''
marker.write_text(previous + 'started\\n')
os._exit(5 if not previous else 0)
"""
            with self.assertLogs('kms_companion.ui_supervisor', level='INFO'):
                result = run([sys.executable, '-c', code, str(marker)], sleep=lambda seconds: None)
            self.assertEqual(result, 0)
            self.assertEqual(marker.read_text().splitlines(), ['started', 'started'])

    def test_native_exit_restarts_but_clean_quit_does_not(self):
        processes = [Mock(pid=1), Mock(pid=2)]
        processes[0].wait.return_value = -1073741819  # Windows access violation.
        processes[1].wait.return_value = 0
        spawn = Mock(side_effect=processes)
        sleep = Mock()
        with self.assertLogs('kms_companion.ui_supervisor', level='INFO'):
            self.assertEqual(run(['tray', '--ui-worker'], spawn=spawn, sleep=sleep, clock=lambda: 10), 0)
        self.assertEqual(spawn.call_count, 2)
        sleep.assert_called_once_with(1)

    def test_repeated_failure_stops_after_three_restarts(self):
        process = Mock(pid=1)
        process.wait.return_value = 5
        spawn, sleep = Mock(return_value=process), Mock()
        with self.assertLogs('kms_companion.ui_supervisor', level='INFO'):
            self.assertEqual(run(['tray'], spawn=spawn, sleep=sleep, clock=lambda: 10), 5)
        self.assertEqual(spawn.call_count, 4)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2, 4])

    def test_clean_exit_never_restarts(self):
        process = Mock(pid=1)
        process.wait.return_value = 0
        spawn, sleep = Mock(return_value=process), Mock()
        self.assertEqual(run(['tray'], spawn=spawn, sleep=sleep), 0)
        spawn.assert_called_once_with(['tray'])
        sleep.assert_not_called()

    def test_restart_budget_recovers_after_a_stable_minute(self):
        process = Mock(pid=1)
        process.wait.side_effect = [5, 5, 5, 5, 0]
        spawn, sleep = Mock(return_value=process), Mock()
        clock = Mock(side_effect=[0, 1, 2, 65])
        with self.assertLogs('kms_companion.ui_supervisor', level='INFO'):
            self.assertEqual(run(['tray'], spawn=spawn, sleep=sleep, clock=clock), 0)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2, 4, 1])


class RuntimeLogTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec('PySide6'), 'Optional UI dependencies not installed')
    def test_unhandled_qt_callback_logs_and_exits_for_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            code = """
import sys
from unittest.mock import patch
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication
from kms_companion.gui import main
app = QApplication([])
def failed_callback():
    raise RuntimeError('unhandled Qt callback marker')
QTimer.singleShot(100, failed_callback)
QTimer.singleShot(5000, lambda: app.exit(99))
sys.argv = ['gui', '--ui-worker', 'ui', '--tray']
with patch('kms_companion.cli.ensure_helper'), \\
     patch('kms_companion.cli.startup_enabled', return_value=False), \\
     patch('kms_companion.ipc.call', side_effect=RuntimeError('Offline test')):
    raise SystemExit(main())
"""
            environment = dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory,
                               QT_QPA_PLATFORM='offscreen')
            result = subprocess.run([sys.executable, '-c', code], env=environment,
                                    capture_output=True, timeout=12)
            self.assertEqual(result.returncode, 70, result.stderr.decode(errors='replace'))
            text = (Path(directory) / 'KMSwitcher/logs/tray.log').read_text(encoding='utf-8')
            self.assertIn('Traceback', text)
            self.assertIn('unhandled Qt callback marker', text)

    def test_main_and_worker_exceptions_retain_traceback_and_build(self):
        with tempfile.TemporaryDirectory() as directory:
            code = """
import threading
from kms_companion.runtime_log import configure
configure('test')
def failed_worker():
    raise RuntimeError('worker failure marker')
worker = threading.Thread(target=failed_worker, name='test-worker')
worker.start()
worker.join()
raise ValueError('main failure marker')
"""
            environment = dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory)
            result = subprocess.run([sys.executable, '-c', code], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            text = (Path(directory) / 'KMSwitcher/logs/test.log').read_text(encoding='utf-8')
            for expected in ('version=', 'commit=', 'Traceback', 'test-worker',
                             'worker failure marker', 'main failure marker'):
                self.assertIn(expected, text)

    def test_fault_output_survives_abrupt_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            code = """
import faulthandler, os
import kms_companion.runtime_log as logs
logs.configure('test')
faulthandler.dump_traceback(file=logs._fault_stream, all_threads=True)
os._exit(5)
"""
            environment = dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory)
            result = subprocess.run([sys.executable, '-c', code], env=environment,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 5)
            text = (Path(directory) / 'KMSwitcher/logs/test-fault.log').read_text(encoding='utf-8')
            self.assertIn('<module>', text)
