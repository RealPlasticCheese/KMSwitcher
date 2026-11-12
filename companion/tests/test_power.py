import asyncio
import os
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import Mock, patch

from kms_companion.agent import Agent
from kms_companion.power import PowerMonitor, PowerState, power_label, windows_event


class PowerTests(unittest.TestCase):
    def test_windows_suspend_resume_shutdown_and_cancel(self):
        self.assertEqual(windows_event(0x218, 4, 0), PowerState.SLEEP_OR_HIBERNATE)
        for event in (7, 18):
            self.assertEqual(windows_event(0x218, event, 0), PowerState.AWAKE)
        self.assertEqual(windows_event(0x11, 0, 0), PowerState.SHUTDOWN)
        for flags in (0x80000000, 1):  # Logging out / application restart is not PC shutdown.
            self.assertIsNone(windows_event(0x11, 0, flags))
        self.assertEqual(windows_event(0x16, 0, 0), PowerState.AWAKE)
        self.assertIsNone(windows_event(0x16, 1, 0))

    def test_stale_report_is_not_presented_as_current_state(self):
        self.assertEqual(power_label("awake", False), "Last reported: Awake")
        self.assertEqual(power_label("shutdown", True), "Shutdown / restart requested")
        self.assertEqual(power_label("unknown", False), "Unknown")

    def test_reporting_does_not_depend_on_automatic_switching_or_desktop(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory):
            agent = Agent()
            agent.enabled = False
            agent.receiver = Mock()
            agent.receiver.status.return_value = {"power_supported": True}
            agent.power_state = PowerState.SLEEP_OR_HIBERNATE
            agent.execute("power-notify", {})
            agent.receiver.power.assert_called_once_with(PowerState.SLEEP_OR_HIBERNATE)
            agent.receiver.reset_mock()
            agent.step()
            agent.receiver.heartbeat.assert_not_called()
            agent.receiver.power.assert_not_called()  # Does not overwrite intent with awake.

    def test_older_receivers_are_not_sent_unknown_commands(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, LOCALAPPDATA=directory, XDG_CONFIG_HOME=directory):
            agent = Agent()
            agent.receiver = Mock()
            agent.report_power({})
            agent.receiver.power.assert_not_called()

    @unittest.skipUnless(sys.platform == "win32", "Windows notification window")
    def test_real_windows_notification_window_starts_and_closes(self):
        events = []
        monitor = PowerMonitor(events.append)
        try:
            monitor.start()
            self.assertTrue(monitor.started.wait(3))
            self.assertEqual(events, [PowerState.AWAKE], monitor.reason)
            self.assertIsNotNone(monitor.window)
        finally:
            monitor.close()
        self.assertFalse(monitor.thread.is_alive())


class LinuxPowerTests(unittest.IsolatedAsyncioTestCase):
    async def test_logind_releases_delay_inhibitor_after_report_and_reacquires_on_resume(self):
        # Exercise the real async monitor against a fake system bus; never suspend the machine.
        events, handles, actions = [], [], []
        read_fd, write_fd = os.pipe()
        monitor = PowerMonitor(events.append)

        class Message:
            def __init__(self, **values):
                self.__dict__.update(values)

        class Bus:
            connected = True
            async def connect(self): return self
            def disconnect(self): self.connected = False
            def add_message_handler(self, handler): self.handler = handler
            async def call(self, message):
                if message.member == "Inhibit":
                    actions.append(message.body[0])
                    fd = os.dup(read_fd)
                    handles.append(fd)
                    return Message(message_type=0, body=[0], unix_fds=[fd])
                return Message(message_type=0, body=[])

        bus = Bus()
        modules = {"dbus_next": types.SimpleNamespace(BusType=types.SimpleNamespace(SYSTEM=1),
            Message=Message, MessageType=types.SimpleNamespace(ERROR=3, SIGNAL=4)),
            "dbus_next.aio": types.SimpleNamespace(MessageBus=lambda **kwargs: bus)}

        async def until(condition):
            for _ in range(100):
                if condition(): return
                await asyncio.sleep(.01)
            self.fail("Power transition did not complete")

        def signal(entering):
            bus.handler(Message(message_type=4, path="/org/freedesktop/login1",
                interface="org.freedesktop.login1.Manager", member="PrepareForSleep", body=[entering]))

        task = None
        try:
            with patch.dict(sys.modules, modules):
                task = asyncio.create_task(monitor._linux())
                await until(lambda: bool(events))
                self.assertEqual(events, [PowerState.AWAKE])
                fds = list(monitor.inhibitor)
                signal(True)
                await until(lambda: monitor.inhibitor is None)
                self.assertEqual(events[-1], PowerState.SLEEP_OR_HIBERNATE)
                for fd in fds:
                    with self.assertRaises(OSError): os.fstat(fd)
                signal(False)
                await until(lambda: events[-1] == PowerState.AWAKE)
                self.assertEqual(len(handles), 4)
                self.assertEqual(actions, ["sleep", "shutdown", "sleep", "shutdown"])
                self.assertIsNotNone(monitor.inhibitor)
                monitor.stopped.set()
                await task
                self.assertIsNone(monitor.inhibitor)
        finally:
            monitor.stopped.set()
            if task and not task.done():
                await task
            os.close(read_fd)
            os.close(write_fd)
