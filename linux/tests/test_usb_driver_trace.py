"""Driver trace import, bounded parsing and reviewed-source guard regressions."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock
from kms_stress.usb_isolation import IsolationRunner, parse_driver_trace, parse_snapshot
from test_usb_isolation import config

class DriverTraceTests(unittest.TestCase):
    def lines(self):
        return ["KMSISO TRACE version=1 bytes=9000 events=1 overwritten=20 samples=1 isrs=300 last-isr=4294967295 frozen=1",
                *[f"KMSISO TCOUNT channel={n} enqueued=1 activated=1 irqs=1 parsed=1 dequeued=1" for n in range(8)],
                "KMSISO TEVENT at=4294967295 frame=ffff1234 kind=1 channel=3 object=3fc12300 detail=00000001 qtd=3fc12000 status=80000040",
                "KMSISO TSAMPLE n=0 at=3 frame=ffff1235 core=0400002d mask=a3000000 channels=00000000 isrs=300 last-isr=4294967295",
                "KMSISO TPIPE sample=0 channel=3 pipe=3fc00000 state=0 endpoint=0581 hal=00000031 multi=00000400 pending=0 done=0 char=814c8840 irq=00000000 mask=00000803 size=400000ff dma=3fc12000 index=0 list=3fc12000 urb=3fc12300 qtd=3fc12000 status=80000040 data=3fc30000 schedule-current=00000008 schedule-next=00000008 schedule-mask=ffffffff"]
    def test_complete_trace_keeps_raw_unsigned_values_and_bitfields(self):
        trace = parse_driver_trace(self.lines())
        self.assertEqual(trace["events"][0]["status"], 0x80000040)
        self.assertEqual(trace["pipes"][0]["schedule-mask"], 0xffffffff)
        self.assertEqual(trace["events"][0]["at"], 0xffffffff)
        self.assertEqual(trace["samples"][0]["at"], 3)
    def test_optional_before_activation_frame_is_hex_and_old_records_remain_valid(self):
        lines = self.lines()
        lines[9] += " before=00081407"
        self.assertEqual(parse_driver_trace(lines)["events"][0]["before"], 0x00081407)
        self.assertNotIn("before", parse_driver_trace(self.lines())["events"][0])

    def test_old_firmware_has_no_trace(self):
        self.assertIsNone(parse_driver_trace(["KMSISO END id=1"]))
    def test_trace_is_inside_fault_frame_and_preserved_in_json(self):
        lines = ["KMSISO BEGIN id=1 boot=00000001 at=3 run=4 seed=1 profile=1 mode=0 watching=1 fault=1 expected=0 devices=0 frozen=1",
                 *self.lines(), "KMSISO END id=1"]
        self.assertEqual(parse_snapshot(lines, 1)["driver_trace"]["summary"]["events"], 1)
    def test_missing_duplicate_overflow_and_unknown_records_fail(self):
        mutations = [lambda l: l.__setitem__(-1, l[-1].replace(" state=0", "")), lambda l: l.pop(0), lambda l: l.append(l[0]), lambda l: l.pop(1),
                     lambda l: l.__setitem__(2, l[1]), lambda l: l.__setitem__(0, l[0].replace("version=1", "version=2")),
                     lambda l: l.__setitem__(0, l[0].replace("events=1", "events=129")),
                     lambda l: l.__setitem__(9, l[9].replace("kind=1", "kind=5")),
                     lambda l: l.__setitem__(9, l[9].replace("channel=3", "channel=8")),
                     lambda l: l.append(l[-1]), lambda l: l.__setitem__(-1, l[-1].replace("sample=0", "sample=1")),
                     lambda l: l.__setitem__(9, l[9].replace("4294967295", "4294967296"))]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                lines = self.lines(); mutate(lines)
                with self.assertRaises(ValueError): parse_driver_trace(lines)
    def test_advertised_trace_cannot_be_silently_omitted(self):
        runner = IsolationRunner(config("unused")); runner.token = 0; runner.boot = 1
        runner.driver_trace_supported = True
        runner.control.master = Mock()
        runner.control.master.request.return_value = [
            "KMSISO BEGIN id=1 boot=00000001 at=3 run=0 seed=1 profile=1 mode=0 watching=1 fault=1 expected=0 devices=0 frozen=1",
            "KMSISO END id=1"]
        with self.assertRaisesRegex(ValueError, "did not return"):
            runner.snapshot(fault=True, service=False)

    def test_overlay_refuses_unreviewed_driver_source(self):
        path = Path(__file__).resolve().parents[2] / "diagnostics/usb-isolation/instrument_hcd.py"
        if not path.exists(): self.skipTest("Source-only overlay generator is outside installed Linux kit")
        spec = importlib.util.spec_from_file_location("instrument_hcd", path)
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        with self.assertRaisesRegex(ValueError, "differs"):
            mod.instrument(b"another USB driver version")
