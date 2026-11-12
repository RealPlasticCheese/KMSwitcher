import os
import tempfile
import time
import unittest
from unittest.mock import patch
from kms_companion.agent import Agent
from kms_companion.ipc import Server, call
from test_companion import FakeDesktop, FakeReceiver


class IpcTests(unittest.TestCase):
    def test_single_owner_cli_commands_and_shutdown(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ,
            {"LOCALAPPDATA": directory, "XDG_CONFIG_HOME": directory}):
            agent = Agent(desktop_factory=FakeDesktop, receiver_factory=lambda serial: FakeReceiver())
            server = Server(agent)
            server.start()
            try:
                self.assertIn("connected", call("status"))
                self.assertEqual(call("auto", enabled=False), {"enabled": False})
                self.assertFalse(call("status")["enabled"])
                with self.assertRaises(RuntimeError):
                    Server(Agent())
                self.assertTrue(call("stop")["stopping"])
            finally:
                server.close()
