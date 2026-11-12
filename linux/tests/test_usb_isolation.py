"""Independent oracle and simulated adapters for the USB-only diagnostic."""
import ctypes
import itertools
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib

from kms_stress.model import Config, Device, pattern
from kms_stress.usb_isolation import IsolationRunner, parse_snapshot, verify_streams
from test_runner_integration import Clock


MASTER = "112233445566"
GENERATORS = ["AABBCCDDEE01", "AABBCCDDEE02"]


class Generator:
    error = None
    def __init__(self, identity):
        self.identity, self.commands, self.closed = identity, [], False
        self.path = identity
        self.run = self.source = self.count = self.crc = self.releases = 0
        self.running = False; self.seed = self.profile = 0
    def log(self, *args): pass
    def request(self, command, prefix=None, timeout=3, **kwargs):
        self.commands.append(command); f = command.split()
        if command == "bench info": return [f"KMSGEN INFO 1 {self.identity} 54 test"]
        if f[:2] == ["bench", "config"]:
            self.run, self.source, self.seed, _, _, self.profile = map(int, f[2:])
        if command == "bench start":
            self.running = True; self.count = self.crc = self.releases = 0
        if command == "bench stats":
            if self.running:
                for _ in range(10):
                    self.count += 1; self.crc = zlib.crc32(pattern(self.source, self.run, self.count, self.seed, self.profile), self.crc)
            return [f"KMSGEN STATS {self.run} {self.source} {self.count} {self.count} {self.count} 0 {self.crc:08x} {int(self.running)} 0"]
        if command == "bench stop": self.running = False; self.releases += 1
        return [prefix] if prefix else []
    def close(self): self.closed = True


class Master:
    error = None
    restarted = False
    supports_workaround = True
    def __init__(self, sources, failure=None):
        self.sources, self.failure, self.commands = sources, failure, []
        self.run = self.seed = self.profile = self.mode = self.watching = self.fault = 0
        self.closed = False
        self.workaround = 1
    def request(self, command, prefix=None, timeout=3, service=None):
        self.commands.append(command)
        if service: service()
        f = command.split()
        if command == "iso info":
            return [f"KMSISO INFO 1 {MASTER if self.failure != 'identity' else '000000000000'} 12345678 test idf=v6.1 usb=1.6.0" + (" workaround=1" if self.supports_workaround else "")]
        if f[:2] == ["iso", "workaround"]: self.workaround = int(f[2])
        if f[:2] == ["iso", "arm"]: self.run, self.seed, self.profile, self.mode, _ = map(int, f[2:])
        if f[:2] == ["iso", "watch"]: self.watching = int(f[2])
        if f[:2] in (["iso", "status"], ["iso", "fault"]):
            if self.watching and self.failure == "fault": self.fault = 1
            token = f[2]
            lines = [f"KMSISO BEGIN id={token} boot={'87654321' if self.watching and self.failure == 'boot' else '12345678'} at=1000 run={self.run} seed={self.seed} profile={self.profile} mode={self.mode} watching={self.watching} fault={self.fault} expected=2 devices=2 frozen={int(bool(self.fault))}"]
            if self.supports_workaround:
                lines.append(f"KMSISO WORKAROUND version=1 enabled={self.workaround} guard-us=20 budget-us=64 activations=1 waited=0 timeouts=0 max-wait-us=0 crossings=0")
            for n, source in enumerate(self.sources):
                count = max(0, source.count - (1 if self.failure == "loss" and self.run else 0))
                lines.append(f"KMSISO DEVICE serial={GENERATORS[n]} address={n+2} source={source.source} count={count} crc={source.crc:08x} sequence-errors=0 payload-errors=0 usb-errors=0 stale=0 releases={source.releases} pending=1 control-pending=0 control-start=0 controls=0 last-report=900")
            return lines + [f"KMSISO END id={token}"]
        return [prefix] if prefix else []
    def close(self): self.closed = True


def config(directory):
    return Config(master=Device(MASTER, uart="master"),
                  generators=[Device(identity, uart=str(n)) for n, identity in enumerate(GENERATORS)],
                  receivers=[], output=directory)


class IsolationTests(unittest.TestCase):
    def execute(self, directory, failure=None, mode="input"):
        cfg = config(directory); sources = [Generator(identity) for identity in GENERATORS]
        master = Master(sources, failure)
        if failure == "generator-identity": sources[1].identity = "000000000000"
        runner = IsolationRunner(cfg, seconds=1, rate=10, mode=mode, notify=lambda _: None)
        adapters = {"master": master, "0": sources[0], "1": sources[1]}
        with patch("kms_stress.usb_isolation.SerialChannel", side_effect=lambda port, log: adapters[port]), \
             patch("kms_stress.usb_isolation.time.monotonic", new=Clock()):
            result, path = runner.run()
        self.assertTrue(master.closed and all(s.closed for s in sources if s.commands))
        self.assertFalse(any(command.startswith(("select", "pause", "test-mode", "bench arm", "usb-recover")) for command in master.commands))
        return result, path, master, sources

    def test_all_modes_verify_delivery_without_receivers(self):
        for mode in ("input", "probe", "led", "both"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                result, path, _, _ = self.execute(directory, mode=mode)
                self.assertIn(result["status"], ("PASS", "LIMITED"))
                self.assertEqual(result["segments"][0]["errors"], [])
                self.assertTrue((path / "isolation-fault.json").exists())
                self.assertEqual(json.loads((path / "result.json").read_text())["coverage"]["radio"], False)

    def test_fault_preserves_snapshot_before_watch_off_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            result, path, master, _ = self.execute(directory, "fault")
            self.assertEqual(result["status"], "FAIL")
            fault = json.loads((path / "isolation-fault.json").read_text())
            self.assertEqual(fault["fault"], 1)
            dump = next(i for i, command in enumerate(master.commands) if command.startswith("iso fault"))
            self.assertLess(dump, master.commands.index("iso watch 0"))

    def test_missing_input_and_boot_changes_never_pass(self):
        for failure, status in (("loss", "FAIL"), ("boot", "INCONCLUSIVE")):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                result, _, _, _ = self.execute(directory, failure)
                self.assertEqual(result["status"], status)

    def test_wrong_master_is_read_only_and_wrong_generator_never_gets_stop(self):
        for failure in ("identity", "generator-identity"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                result, _, master, sources = self.execute(directory, failure)
                self.assertEqual(result["status"], "INCONCLUSIVE")
                if failure == "identity": self.assertEqual(master.commands, ["", "iso info"])
                else: self.assertEqual(sources[1].commands, ["", "bench info"])

    def test_invalid_parameters_rejected_before_hardware(self):
        for kwargs in ({"seconds": 0}, {"seconds": 86400}, {"rate": 0}, {"rate": 1001}, {"mode": "other"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError): IsolationRunner(config("unused"), **kwargs)
        cfg = config("unused"); cfg.generators[0].uart = "master"
        with self.assertRaises(ValueError): IsolationRunner(cfg)

    def test_token_frame_malformed_fields_and_completion_mismatches(self):
        sources = [Generator(identity) for identity in GENERATORS]; master = Master(sources)
        lines = master.request("iso status 42")
        self.assertEqual(parse_snapshot(lines, 42)["devices"], 2)
        for bad in (lines[:-1], lines + lines, [line.replace("id=42", "id=41") for line in lines],
                    [line.replace("pending=1", "pending=2") for line in lines],
                    [line.replace("crc=00000000", "crc=zzzz") for line in lines],
                    [line.replace("devices=2", "devices=3") for line in lines]):
            with self.subTest(bad=bad[:1]), self.assertRaises(ValueError): parse_snapshot(bad, 42)
        state = parse_snapshot(lines, 42)
        self.assertTrue(verify_streams(state, {}, config("unused").generators))


class Stream(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("run", "seed", "profile", "source", "count", "crc", "sequence_errors", "payload_errors", "stale", "releases")]


@unittest.skipUnless(os.environ.get("KMS_STRESS_PATTERN_DLL"), "Native production verifier DLL supplied by test-stress.py")
class ProductionOracleTests(unittest.TestCase):
    def test_production_verifier_crc_matches_independent_python_oracle(self):
        native = ctypes.CDLL(os.environ["KMS_STRESS_PATTERN_DLL"])
        native.iso_stream_report.argtypes = [ctypes.POINTER(Stream), ctypes.c_void_p, ctypes.c_size_t]
        for source, profile, seed in itertools.product((1, 2), range(5), (0, 123, 0xffffffff)):
            with self.subTest(source=source, profile=profile, seed=seed):
                state = Stream(run=0xabcd1234, seed=seed, profile=profile); crc = 0
                for sequence in range(1, 101):
                    payload = pattern(source, state.run, sequence, seed, profile)
                    data = ctypes.create_string_buffer(b"\x01" + payload)
                    native.iso_stream_report(ctypes.byref(state), data, 55); crc = zlib.crc32(payload, crc)
                self.assertEqual((state.count, state.crc, state.payload_errors, state.sequence_errors), (100, crc, 0, 0))


if __name__ == "__main__": unittest.main()
