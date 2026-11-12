"""Device-side evidence framing, cleanup order and legacy compatibility."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_stress.hardware import read_identity
from kms_stress.usb_isolation import IsolationRunner, parse_generator_trace, parse_driver_trace
from test_usb_isolation import Clock, Generator, Master, GENERATORS, config
import test_usb_driver_trace as driver_tests


def trace_lines(token=42, identity=GENERATORS[0], run=123):
    return [f"KMSGEN TRACE id={token} serial={identity} version=1 run={run} frozen=1 events=1 samples=1 overwritten=0 entries=30 exits=30 last-enter=1 max-isr=8 arms=3 accepted=3 callbacks=2 resets=0 suspended=0 resumed=0",
            "KMSGEN GEVENT n=0 at=4294967295 kind=0 status=80000000 mask=00000001 device=00000002 endpoints=00000002 ctl=80000000 irq=00000000 size=00080037 dma=00000000 fifo=00000020 ahb=00000001 detail=3fc12345 value=00370040",
            "KMSGEN GSAMPLE n=0 at=3 kind=4 status=00000000 mask=00000001 device=00000002 endpoints=00000002 ctl=80000000 irq=00000000 size=00080037 dma=00000000 fifo=00000020 ahb=00000001 detail=00000000 value=00000000",
            f"KMSGEN TRACEEND id={token}"]


class GeneratorTraceTests(unittest.TestCase):
    def test_optional_identity_capability_and_legacy(self):
        for suffix, expected in [("", False), (" trace=1", True)]:
            channel=Mock(); channel.request.return_value=[f"KMSGEN INFO 1 {GENERATORS[0]} 54 git{suffix}"]
            self.assertEqual(read_identity(channel).trace, expected)
        for suffix in [" trace=0", " trace=2", " trace=1 surprise"]:
            channel=Mock(); channel.request.return_value=[f"KMSGEN INFO 1 {GENERATORS[0]} 54 git{suffix}"]
            with self.assertRaises(ValueError): read_identity(channel)

    def test_unsigned_device_registers_and_counters(self):
        data=parse_generator_trace(trace_lines(),42,GENERATORS[0],123)
        self.assertEqual(data["events"][0]["ctl"],0x80000000)
        self.assertEqual(data["events"][0]["value"],0x00370040)
        self.assertEqual(data["summary"]["callbacks"],2)

    def test_incomplete_duplicate_wrong_run_and_out_of_bounds_rejected(self):
        lines=trace_lines()
        cases=[lines[:-1], lines+lines, [l.replace("run=123","run=124") for l in lines],
               [l.replace(GENERATORS[0],GENERATORS[1]) for l in lines],
               [l.replace("events=1","events=65") for l in lines],
               [l.replace("samples=1","samples=7") for l in lines],
               [l.replace("kind=0","kind=5") for l in lines],
               [l.replace("n=0","n=1") for l in lines],
               [l.replace("4294967295","4294967296") for l in lines],
               [l.replace(" callbacks=2","") for l in lines],
               [l.replace(" fifo=00000020","") for l in lines]]
        for bad in cases:
            with self.subTest(bad=bad),self.assertRaises(ValueError):
                parse_generator_trace(bad,42,GENERATORS[0],123)

    def test_two_buffer_records_are_required_when_advertised(self):
        lines=driver_tests.DriverTraceTests().lines(); lines[0]+=" buffers=2"
        buffers=[f"KMSISO TBUFFER sample=0 channel=3 slot={slot} list=3fc12000 urb=3fc12300 flags=00000001 state=00000000 first=86000040 second=00000000 index=0 current=86000040 data=3fc30000" for slot in range(2)]
        self.assertEqual(len(parse_driver_trace(lines+buffers)["buffers"]),2)
        for bad in [lines,lines+buffers[:1],lines+buffers[:1]*2,
                    lines+[b.replace("slot=1","slot=2") for b in buffers]]:
            with self.subTest(bad=bad),self.assertRaises(ValueError): parse_driver_trace(bad)

    def test_freeze_all_sources_before_host_dump_and_dump_before_cleanup(self):
        actions=[]
        class TracedGenerator(Generator):
            def request(self,command,prefix=None,**kwargs):
                actions.append((self.identity,command))
                if command=="bench info": return [f"KMSGEN INFO 1 {self.identity} 54 git trace=1"]
                if command.startswith("bench trace freeze "):
                    return [f"KMSGEN TRACEFROZEN id={command.split()[-1]}"]
                if command.startswith("bench trace dump "):
                    return trace_lines(int(command.split()[-1]),self.identity,self.run)
                return super().request(command,prefix,**kwargs)
        class TracedMaster(Master):
            def request(self,command,*args,**kwargs):
                actions.append(("master",command)); return super().request(command,*args,**kwargs)
        with tempfile.TemporaryDirectory() as directory:
            sources=[TracedGenerator(i) for i in GENERATORS]; master=TracedMaster(sources,"fault")
            adapters={"master":master,"0":sources[0],"1":sources[1]}
            runner=IsolationRunner(config(directory),seconds=1,rate=10,notify=lambda _:None)
            with patch("kms_stress.usb_isolation.SerialChannel",side_effect=lambda p,l:adapters[p]), \
                 patch("kms_stress.usb_isolation.time.monotonic",new=Clock()):
                result,path=runner.run()
            self.assertEqual(result["status"],"FAIL"); self.assertEqual(result["cleanup"],[])
            fault=next(i for i,(_,cmd) in enumerate(actions) if cmd.startswith("iso fault"))
            for identity in GENERATORS:
                freeze=next(i for i,(who,cmd) in enumerate(actions) if who==identity and cmd.startswith("bench trace freeze"))
                dump=next(i for i,(who,cmd) in enumerate(actions) if who==identity and cmd.startswith("bench trace dump"))
                self.assertLess(freeze,fault); self.assertGreater(dump,fault)
                self.assertTrue(any(who==identity and cmd=="bench stop" for who,cmd in actions[dump+1:]))
                data=json.loads((path/f"generator-trace-{identity}.json").read_text())
                self.assertEqual(data["summary"]["run"],runner.run_id)

    def test_overlay_guards_refuse_different_library_sources(self):
        root=Path(__file__).resolve().parents[2]
        for file in ["diagnostics/hid-generator/instrument_dcd.py","diagnostics/usb-isolation/instrument_hal.py"]:
            path=root/file
            if not path.exists(): self.skipTest("Overlay sources outside installed Linux kit")
            spec=importlib.util.spec_from_file_location("overlay",path)
            mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
            with self.assertRaisesRegex(ValueError,"Unreviewed"): mod.instrument(b"unexpected library revision")
