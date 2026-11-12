"""Headless discovery must stay usable through SSH without importing Tk."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from kms_stress import __version__
from kms_stress.__main__ import main
from kms_stress.model import Config, Device


class CommandLineTests(unittest.TestCase):
    def test_recipe_override_reaches_runner_without_gui_or_hardware(self):
        from dataclasses import asdict
        import json
        from kms_stress.campaign import Case, fixture
        with tempfile.TemporaryDirectory() as directory:
            cfg = Config(master=Device("112233445566", uart="master"),
                         generators=[Device("AABBCCDDEE01", uart="a")],
                         receivers=[Device("AABBCCDDEE06", slot=6)])
            profile = Path(directory) / "profile.json"; cfg.save(profile)
            case = Case(6, 25, 5, 17, 1, "stress", "console")
            recipe = Path(directory) / "recipe.json"
            recipe.write_text(json.dumps({"version": 1, "fixture": fixture(cfg), "case_id": case.identity, "case": asdict(case)}))
            with patch.object(sys, "argv", ["kms-stress", "--run", "--profile", str(profile), "--replay-recipe", str(recipe)]), \
                 patch("kms_stress.runner.Runner") as runner, patch("signal.signal"), \
                 patch.dict(sys.modules, {"kms_stress.gui": None}):
                runner.return_value.run.return_value = ({"status": "PASS"}, Path(directory))
                self.assertEqual(main(), 0)
            actual = runner.call_args.args[0]
            self.assertEqual(actual.preset, "usb-stall")
            self.assertEqual(json.loads(actual.campaign_recipe)["case_id"], case.identity)
            self.assertEqual(Config.load(profile).preset, "quick")

    def test_replay_requires_explicit_run_or_web(self):
        with patch.object(sys, "argv", ["kms-stress", "--replay-recipe", "not-opened.json"]), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            main()
        self.assertEqual(error.exception.code, 2)

    def test_discovery_prints_version_and_uart_diagnostics_without_gui(self):
        def discover(log):
            log("uart", {"port": "bridge", "line": "KMSBENCH INFO 1 master AABBCCDDEEFF 12345678 0"})
            return [("master", Device("AABBCCDDEEFF", uart="bridge"))], []
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(sys, "argv", ["kms-stress", "--discover", "--profile", str(Path(directory) / "profile.json")]), \
             patch.dict(sys.modules, {"kms_stress.gui": None}), \
             patch("kms_stress.hardware.discover_serial", side_effect=discover), \
             patch("kms_stress.hardware.receiver_nodes", return_value={}), \
             contextlib.redirect_stdout(output):
            self.assertEqual(main(), 0)
        self.assertIn(f"hardware test {__version__}", output.getvalue())
        self.assertIn("bridge [uart]: KMSBENCH INFO", output.getvalue())
        self.assertIn("master AABBCCDDEEFF bridge", output.getvalue())


if __name__ == "__main__": unittest.main()
