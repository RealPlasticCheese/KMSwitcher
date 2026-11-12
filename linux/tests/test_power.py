"""Packaged sessions release the sleep lease after normal and interrupted runs."""
import signal
import unittest
from unittest.mock import Mock, patch
from kms_stress import power


class PowerTests(unittest.TestCase):
    def test_packaged_sleep_helper_is_released_even_after_a_test_failure(self):
        process = Mock(pid=12345); process.poll.return_value = None
        with patch.object(power.sys, "platform", "linux"), \
             patch.object(power.shutil, "which", return_value="/usr/bin/systemd-inhibit"), \
             patch.object(power.subprocess, "run", return_value=Mock(returncode=0)), \
             patch.object(power.subprocess, "Popen", return_value=process) as start, \
             patch.object(power.os, "killpg", create=True) as stop:
            with self.assertRaisesRegex(RuntimeError, "test failed"):
                with power.inhibit_sleep(): raise RuntimeError("test failed")
            self.assertTrue(start.call_args.kwargs["start_new_session"])
            self.assertEqual(start.call_args.kwargs["stdin"], power.subprocess.PIPE)
            process.stdin.close.assert_called_once()
            stop.assert_not_called()
            process.wait.assert_called_once_with(timeout=5)

    def test_source_launchers_and_demo_sessions_do_not_spawn_another_inhibitor(self):
        with patch.object(power.subprocess, "Popen") as start:
            with power.inhibit_sleep(enabled=False): pass
            start.assert_not_called()
