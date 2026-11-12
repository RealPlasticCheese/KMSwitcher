"""Verify restart targeting, reset confirmation, cancellation and actionable errors."""
import json
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from kms_stress.hardware import FirmwareIdentity
from kms_stress.model import Config, Device
from kms_stress.restarts import (RestartCancelled, restart_devices, restart_receiver,
                                restart_serial, restart_targets)
from kms_stress.runner import Cancelled, Runner


def config():
    return Config(master=Device("112233445566", "Master", "master"),
                  generators=[Device("AABBCCDDEE01", "Generator", "generator"),
                              Device("AABBCCDDEE03", "Unused generator", "unused", enabled=False)],
                  receivers=[Device("AABBCCDDEE02", "Office receiver", slot=1)])


class RestartTests(unittest.TestCase):
    def test_restart_all_and_automatic_restart_have_explicit_scopes(self):
        c = config()
        self.assertEqual(len(restart_targets(c)), 4)
        self.assertEqual(len(restart_targets(c, enabled_only=True)), 3)
        c.preset = "baseline"
        self.assertEqual([role for role, _ in restart_targets(c, enabled_only=True)], ["generator"])
        c.generators[1].identity = c.generators[0].identity
        with self.assertRaisesRegex(ValueError, "distinct"):
            restart_targets(c)

    def test_unknown_or_ambiguous_target_is_rejected_before_opening_hardware(self):
        with patch("kms_stress.restarts.SerialChannel") as channel:
            for target in ({"role":"receiver", "identity":"unknown"}, {"uart":"arbitrary"}):
                with self.assertRaises(ValueError): restart_devices(config(), target=target)
        channel.assert_not_called()

    def test_identity_mismatch_prevents_every_restart_and_closes_channels(self):
        channel = Mock()
        with patch("kms_stress.restarts.SerialChannel", return_value=channel), \
             patch("kms_stress.restarts.read_identity", return_value=FirmwareIdentity("master", "FFFFFFFFFFFF")), \
             patch("kms_stress.restarts.restart_serial") as reset:
            with self.assertRaisesRegex(ValueError, "no devices were restarted"):
                restart_devices(config())
        reset.assert_not_called()
        channel.request.assert_not_called()
        channel.close.assert_called_once()

    def test_old_receiver_firmware_is_rejected_before_resetting_other_devices(self):
        c = config()
        def identity(ch):
            role, serial = {"master": ("master", c.master.identity), "generator": ("generator", c.generators[0].identity),
                            "unused": ("generator", c.generators[1].identity)}[ch.path]
            return FirmwareIdentity(role, serial)
        with patch("kms_stress.restarts.SerialChannel", side_effect=lambda path, _: Mock(path=path)), \
             patch("kms_stress.restarts.read_identity", side_effect=identity), \
             patch("kms_stress.restarts.receiver_control", return_value=7), \
             patch("kms_stress.restarts.receiver_page", return_value=(1, 0)), \
             patch("kms_stress.restarts.os.close"), patch("kms_stress.restarts.restart_serial") as reset:
            with self.assertRaisesRegex(RuntimeError, "Office receiver needs updated receiver firmware"):
                restart_devices(c)
        reset.assert_not_called()

    def test_receiver_waits_for_a_changed_boot_and_closes_every_handle(self):
        cancel = Mock()
        cancel.wait.return_value = False
        ioctl = Mock()
        with patch.dict(sys.modules, {"fcntl":types.SimpleNamespace(ioctl=ioctl)}), \
             patch("kms_stress.restarts.receiver_control", side_effect=[7, 8, 9]), \
             patch("kms_stress.restarts.receiver_page", side_effect=[(1,16), (1,16), (2,16)]), \
             patch("kms_stress.restarts.os.close") as close:
            result = restart_receiver(config().receivers[0], cancel, Mock())
        self.assertEqual(result["after"], "00000002")
        self.assertEqual([call.args[0] for call in close.call_args_list], [7, 8, 9])
        self.assertEqual(ioctl.call_args.args[2][1:5], b"KMT1")

    def test_serial_reset_releases_boot_and_en_even_when_cancelled(self):
        channel = Mock()
        cancel = Mock()
        cancel.wait.return_value = True
        with patch("kms_stress.restarts.read_identity", return_value=FirmwareIdentity("master", config().master.identity, "00000001")), \
             patch("kms_stress.restarts.time.sleep"):
            with self.assertRaises(RestartCancelled):
                restart_serial("master", config().master, channel, cancel, Mock())
        self.assertFalse(channel.port.dtr)
        self.assertFalse(channel.port.rts)

    def test_serial_restart_confirms_same_chip_and_new_master_boot(self):
        c = config()
        cancel = Mock(); cancel.wait.return_value = False
        with patch("kms_stress.restarts.read_identity", side_effect=[FirmwareIdentity("master", c.master.identity, "00000001"),
                  FirmwareIdentity("master", c.master.identity, "00000001"), FirmwareIdentity("master", c.master.identity, "00000002")]), \
             patch("kms_stress.restarts.time.sleep"):
            result = restart_serial("master", c.master, Mock(), cancel, Mock())
        self.assertEqual(result["after"], "00000002")

    def test_selection_error_names_the_configured_receiver_and_preserves_details(self):
        runner = Runner(config())
        runner.master = Mock()
        runner.master.request.side_effect = RuntimeError("KMSROUTE ERROR selection unavailable")
        with self.assertRaisesRegex(RuntimeError, "Try restarting Office receiver.*KMSROUTE ERROR selection unavailable"):
            runner.select_receiver(1)

    def test_automatic_restart_runs_before_preflight_and_saves_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            c = config(); c.output = directory; c.restart_before_run = True
            runner = Runner(c)
            events = []
            def restart(*args, **kwargs): events.append("restart"); return [{"role":"receiver"}]
            def prepare(): events.append("preflight"); raise Cancelled("Stopped after preparation")
            with patch("kms_stress.runner.restart_devices", side_effect=restart), patch.object(runner, "preflight", side_effect=prepare):
                result, path = runner.run()
            self.assertEqual(events, ["restart", "preflight"])
            self.assertEqual(result["status"], "CANCELLED")
            self.assertEqual(json.loads((path/"device-restarts.json").read_text()), [{"role":"receiver"}])


if __name__ == "__main__": unittest.main()
