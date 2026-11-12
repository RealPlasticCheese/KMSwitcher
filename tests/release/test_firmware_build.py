"""Verify that portable firmware builds preserve pairing and release settings."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("kms_idf", ROOT / "scripts/idf.py")
idf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(idf)


class FirmwareBuildTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.sdk = self.root / "sdk"
        (self.sdk / "tools").mkdir(parents=True)
        (self.sdk / "tools/idf.py").touch()
        (self.root / "toolchain.json").write_text(json.dumps({"espIdfVersion": "v6.1"}))
        self.commands = []
        self.environment = patch.dict(os.environ, {"IDF_PATH": str(self.sdk)})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def run_command(self, command, **kwargs):
        self.commands.append(command)
        Path(command[command.index("-B") + 1]).mkdir(parents=True, exist_ok=True)

    def output(self, command, **kwargs):
        return "ESP-IDF v6.1" if command[-1] == "--version" else "size report\n"

    def test_release_builds_slave_first_and_embeds_that_build_in_master(self):
        with patch.object(idf.subprocess, "run", side_effect=self.run_command), \
             patch.object(idf.subprocess, "check_output", side_effect=self.output):
            idf.build("controller", release=True, root=self.root)
        self.assertEqual([Path(c[c.index("-C") + 1]).name for c in self.commands], ["receiver", "controller"])
        for command, role, chip in zip(self.commands, ("receiver", "controller"), ("esp32s2", "esp32s3")):
            self.assertIn("IDF_TARGET=" + chip, command)
            self.assertEqual(Path(command[command.index("-B") + 1]), self.root / "build/production" / role)
            defaults = next(item for item in command if item.startswith("SDKCONFIG_DEFAULTS="))
            self.assertIn("sdkconfig.release", defaults)
            self.assertIn("sdkconfig." + role, defaults)
            self.assertEqual((self.root / "build/production" / role / "size.json").read_text(), "size report\n")
        self.assertIn("KMS_RECEIVER_BUILD=" + (self.root / "build/production/receiver").as_posix(), self.commands[1])

    def test_failed_slave_prevents_master_build(self):
        with patch.object(idf.subprocess, "check_output", side_effect=self.output), \
             patch.object(idf.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "slave")) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                idf.build("controller", release=True, root=self.root)
        self.assertEqual(run.call_count, 1)
        self.assertIn(str(self.root / "firmware/receiver"), run.call_args.args[0])

    def test_diagnostic_uses_separate_output_and_defaults(self):
        with patch.object(idf.subprocess, "run", side_effect=self.run_command), \
             patch.object(idf.subprocess, "check_output", side_effect=self.output):
            idf.build("hid-generator", root=self.root)
        self.assertEqual(len(self.commands), 1)
        command = self.commands[0]
        self.assertIn(str(self.root / "build/hid-generator-esp32s3"), command)
        defaults = next(item for item in command if item.startswith("SDKCONFIG_DEFAULTS="))
        self.assertIn("sdkconfig.hid-generator.esp32s3", defaults)
        self.assertNotIn("sdkconfig.release", defaults)

    def test_wrong_sdk_is_rejected_before_build(self):
        with patch.object(idf.subprocess, "check_output", return_value="ESP-IDF v5.5"), \
             patch.object(idf.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "Expected ESP-IDF"):
                idf.build("controller", root=self.root)
        run.assert_not_called()

    def test_wrong_target_and_implicit_hardware_access_are_rejected(self):
        for args in ({"target": "esp32s2"}, {"action": "flash"}, {"action": "monitor"}):
            with self.subTest(args=args), self.assertRaises(ValueError):
                idf.build("controller", root=self.root, **args)

    def test_firmware_job_stays_on_one_linux_agent(self):
        import yaml
        pipeline = yaml.safe_load((ROOT / "pipelines/firmware.yml").read_text())
        self.assertEqual(len(pipeline["jobs"]), 1)
        job = pipeline["jobs"][0]
        self.assertEqual(job["pool"]["${{ if eq(parameters.runLinuxOnAzure, true) }}"]["vmImage"], "ubuntu-22.04")
        self.assertEqual(job["pool"]["${{ else }}"]["name"], "SelfHosted-Linux")
        steps = json.dumps(job["steps"])
        self.assertIn("--project controller --release", steps)
        self.assertNotIn("idf.ps1", steps)
        self.assertNotIn('"download"', steps)
