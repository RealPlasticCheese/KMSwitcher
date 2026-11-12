"""Boolean CLI, negotiated firmware mode and matched control-run evidence."""
import json
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_stress.usb_isolation import IsolationRunner, main, parse_snapshot
from test_usb_isolation import Clock, Generator, Master, GENERATORS, config


class WorkaroundTests(unittest.TestCase):
    def test_cli_defaults_true_and_parses_false_as_false(self):
        for args, enabled in [([],True),(["--workaround","true"],True),(["--workaround","false"],False)]:
            with self.subTest(args=args), patch("sys.argv",["usb-isolation",*args]), \
                 patch("kms_stress.usb_isolation.Config.load",return_value=config("unused")), \
                 patch("kms_stress.usb_isolation.signal.signal"), \
                 patch("kms_stress.usb_isolation.IsolationRunner") as runner:
                runner.return_value.run.return_value=({"status":"PASS"},None)
                self.assertEqual(main(),0)
                self.assertIs(runner.call_args.kwargs["workaround"],enabled)

    def test_invalid_cli_and_nonboolean_api_are_rejected(self):
        for value in ["yes","0","1",""]:
            with self.subTest(value=value),patch("sys.argv",["usb-isolation","--workaround",value]), \
                 patch("kms_stress.usb_isolation.Config.load") as load, \
                 patch("sys.stderr"),self.assertRaises(SystemExit): main()
            load.assert_not_called()
        for value in ["false",0,1,None]:
            with self.subTest(value=value),self.assertRaises(ValueError): IsolationRunner(config("unused"),workaround=value)

    def execute(self,directory,enabled=True,legacy=False,ignore=False,omit=False,crossing=False):
        class TestMaster(Master):
            supports_workaround=not legacy
            def request(self,command,*args,**kwargs):
                before=self.workaround
                lines=super().request(command,*args,**kwargs)
                if ignore and command.startswith("iso workaround"): self.workaround=before
                if omit: lines=[l for l in lines if not l.startswith("KMSISO WORKAROUND ")]
                if crossing: lines=[l.replace("crossings=0","crossings=1") for l in lines]
                return lines
        sources=[Generator(i) for i in GENERATORS]; master=TestMaster(sources)
        adapters={"master":master,"0":sources[0],"1":sources[1]}
        runner=IsolationRunner(config(directory),seconds=1,rate=10,workaround=enabled,notify=lambda _:None)
        with patch("kms_stress.usb_isolation.SerialChannel",side_effect=lambda p,l:adapters[p]), \
             patch("kms_stress.usb_isolation.time.monotonic",new=Clock()): result,path=runner.run()
        return result,path,master,sources

    def test_both_modes_applied_before_arm_and_retained_in_results(self):
        for enabled in [False,True]:
            with self.subTest(enabled=enabled),tempfile.TemporaryDirectory() as directory:
                result,path,master,_=self.execute(directory,enabled)
                self.assertIn(result["status"],["PASS","LIMITED"])
                self.assertIs(result["workaround"],enabled)
                self.assertEqual(result["segments"][0]["workaround_diagnostics"]["enabled"],int(enabled))
                selected=master.commands.index(f"iso workaround {int(enabled)}")
                armed=next(i for i,c in enumerate(master.commands) if c.startswith("iso arm "))
                self.assertLess(selected,armed)
                self.assertEqual(json.loads((path/"isolation-inventory.json").read_text())["snapshot"]["workaround"]["enabled"],int(enabled))

    def test_legacy_requires_explicit_false_and_true_is_read_only(self):
        for enabled in [False,True]:
            with self.subTest(enabled=enabled),tempfile.TemporaryDirectory() as directory:
                result,_,master,sources=self.execute(directory,enabled,legacy=True)
                if enabled:
                    self.assertEqual(result["status"],"INCONCLUSIVE")
                    self.assertEqual(master.commands,["","iso info"])
                    self.assertFalse(any(s.commands for s in sources))
                    self.assertIn("--workaround false",result["message"])
                else:
                    self.assertIn(result["status"],["PASS","LIMITED"])
                    self.assertFalse(any(c.startswith("iso workaround") for c in master.commands))

    def test_ignored_setting_and_missing_advertised_state_prevent_start(self):
        for extra in [{"ignore":True},{"omit":True}]:
            with self.subTest(extra=extra),tempfile.TemporaryDirectory() as directory:
                result,_,master,_=self.execute(directory,False,**extra)
                self.assertEqual(result["status"],"INCONCLUSIVE")
                self.assertFalse(any(c.startswith("iso arm ") for c in master.commands))

    def test_unprotected_crossing_cannot_pass_as_guarded_success(self):
        with tempfile.TemporaryDirectory() as directory:
            result,_,_,_=self.execute(directory,crossing=True)
            self.assertEqual(result["status"],"LIMITED")
            self.assertIn("could not protect",result["message"])

    def test_invalid_duplicate_and_incomplete_guard_records_are_rejected(self):
        sources=[Generator(i) for i in GENERATORS]; lines=Master(sources).request("iso status 42")
        guard=next(l for l in lines if l.startswith("KMSISO WORKAROUND "))
        for bad_guard in [guard.replace("enabled=1","enabled=2"),guard.replace("version=1","version=2"),
                          guard.replace("guard-us=20","guard-us=0"),guard.replace(" timeouts=0","")]:
            bad=[bad_guard if l==guard else l for l in lines]
            with self.subTest(guard=bad_guard),self.assertRaises(ValueError): parse_snapshot(bad,42)
        with self.assertRaises(ValueError): parse_snapshot(lines[:-1]+[guard,lines[-1]],42)
