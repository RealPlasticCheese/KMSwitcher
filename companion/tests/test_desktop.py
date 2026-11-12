"""Frozen desktop tools must use host libraries without weakening lock checks."""
import os
import subprocess
import sys
import unittest
from unittest.mock import patch

from kms_companion.desktop import X11Desktop, _system_environment


class DesktopEnvironmentTests(unittest.TestCase):
    def test_source_process_environment_is_preserved(self):
        environment = dict(DISPLAY=":0", LD_LIBRARY_PATH="/custom/lib")
        with patch.dict(os.environ, environment, clear=True), patch.object(sys, "frozen", False, create=True):
            self.assertEqual(_system_environment(), environment)

    def test_frozen_process_without_original_path_uses_host_defaults(self):
        environment = dict(DISPLAY=":0", DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/1000/bus",
                           LD_LIBRARY_PATH="/app/_internal")
        with patch.dict(os.environ, environment, clear=True), \
             patch.object(sys, "frozen", True, create=True), patch.object(sys, "_MEIPASS", "/app/_internal", create=True):
            result = _system_environment()
            self.assertNotIn("LD_LIBRARY_PATH", result)
            self.assertEqual(result["DISPLAY"], ":0")
            self.assertEqual(result["DBUS_SESSION_BUS_ADDRESS"], environment["DBUS_SESSION_BUS_ADDRESS"])
            self.assertEqual(dict(os.environ), environment)

    def test_nested_frozen_helper_preserves_only_original_host_paths(self):
        original = os.pathsep.join(("/app/_internal", "/custom/lib", "/app/_internal/PySide6/Qt/lib"))
        with patch.dict(os.environ, dict(LD_LIBRARY_PATH="/app/_internal", LD_LIBRARY_PATH_ORIG=original), clear=True), \
             patch.object(sys, "frozen", True, create=True), patch.object(sys, "_MEIPASS", "/app/_internal", create=True):
            result = _system_environment()
        self.assertEqual(result, dict(LD_LIBRARY_PATH="/custom/lib"))


class X11ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.desktop = X11Desktop.__new__(X11Desktop)
        self.desktop.lock_checked, self.desktop.unlocked = 0, False
        self.environment = dict(DISPLAY=":0", XDG_SESSION_ID="c2", XDG_CURRENT_DESKTOP="X-Cinnamon",
                                LD_LIBRARY_PATH="/app/_internal", LD_LIBRARY_PATH_ORIG="/app/_internal")
        patches = (patch.dict(os.environ, self.environment, clear=True),
                   patch.object(sys, "frozen", True, create=True),
                   patch.object(sys, "_MEIPASS", "/app/_internal", create=True),
                   patch("kms_companion.desktop.time.monotonic", return_value=10))
        for replacement in patches:
            replacement.start()
            self.addCleanup(replacement.stop)

    def test_monitor_and_unlock_queries_use_host_environment(self):
        with patch("kms_companion.desktop.subprocess.check_output", side_effect=[
                "Monitors: 1\n 0: +*HDMI-1 1366/344x768/194+0+0 HDMI-1\n",
                "Active=yes\nLockedHint=no\n", "(false,)\n"]) as output:
            self.assertEqual(self.desktop.screens()[0].width, 1366)
            self.assertTrue(self.desktop.ready())
        self.assertEqual([call.args[0][0] for call in output.call_args_list], ["xrandr", "loginctl", "gdbus"])
        for call in output.call_args_list:
            self.assertNotIn("LD_LIBRARY_PATH", call.kwargs["env"])
            self.assertNotIn("LD_LIBRARY_PATH_ORIG", call.kwargs["env"])
            self.assertEqual(call.kwargs["env"]["XDG_SESSION_ID"], "c2")

    def test_locked_or_inactive_session_is_not_ready(self):
        for info in ("Active=yes\nLockedHint=yes\n", "Active=no\nLockedHint=no\n"):
            with self.subTest(info=info), patch("kms_companion.desktop.subprocess.check_output", return_value=info) as output:
                self.desktop.lock_checked = 0
                self.assertFalse(self.desktop.ready())
                output.assert_called_once()

    def test_active_screensaver_is_not_ready(self):
        with patch("kms_companion.desktop.subprocess.check_output", side_effect=["Active=yes\nLockedHint=no\n", "(true,)\n"]):
            self.assertFalse(self.desktop.ready())

    def test_failed_screensaver_query_is_not_ready(self):
        with patch("kms_companion.desktop.subprocess.check_output", side_effect=[
                "Active=yes\nLockedHint=no\n", subprocess.CalledProcessError(127, "gdbus")]):
            self.assertFalse(self.desktop.ready())
