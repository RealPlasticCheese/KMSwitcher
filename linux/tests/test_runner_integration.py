"""Drive the real runner, oracle and reporting through simulated device adapters."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from kms_stress.model import Config, Device, Stream, pattern
from kms_stress.runner import Evidence, Runner


class Clock:
    def __init__(self): self.now = 100.0
    def __call__(self): self.now += 0.05; return self.now


class Fixture:
    def __init__(self, runner, lose_linux_keyboard=False):
        self.runner, self.lose = runner, lose_linux_keyboard
        self.states = {1: bytes(54), 2: bytes(54)}
        self.keyboard = Stream(previous=bytes(34))
        self.raw, self.run = [], 0
        self.events = []
        self.sources = [Source(self, 1), Source(self, 2)]
        self.lost = False

    def emit(self, source, sequence):
        gen = self.sources[source - 1]
        payload = pattern(source, self.run, sequence, gen.seed, gen.profile)
        self.raw.append({"stage": 1, "data": (b"\x01" + payload).hex()})
        self.states[source] = payload
        keys = mods = buttons = 0
        for state in self.states.values():
            keys |= int.from_bytes(state[1:33], "little"); mods |= state[0]
            buttons |= int.from_bytes(state[33:35], "little")
        keyboard = bytes([mods, 0]) + keys.to_bytes(32, "little")
        if keyboard != self.keyboard.previous:
            self.keyboard.add(keyboard)
            if self.lose and not self.lost:
                self.lost = True
            else: self.runner.consume(6, 0, keyboard, 100)
        self.runner.consume(6, 1, struct.pack("<H", buttons) + payload[35:41], 101)

    def traces(self, receiver=False):
        summary = {"reference_keys_count": self.keyboard.count, "reference_keys_crc": self.keyboard.crc,
                   "routed_keys_count": self.keyboard.count, "routed_keys_crc": self.keyboard.crc,
                   "completed_keys_count": self.keyboard.count, "completed_keys_crc": self.keyboard.crc,
                   "submitted": 20, "completed": 20, "faults": 0}
        for source in self.sources:
            current = source.started_run == self.run
            summary[f"source{source.source}_count"] = source.count if current else 0
            summary[f"source{source.source}_crc"] = source.crc if current else 0
        return {"run": self.run, "boot": "12345678", "overwritten": 0, "records": self.raw, "summary": summary}


class Source:
    error = None
    def __init__(self, fixture, source):
        self.fixture, self.source, self.count, self.crc, self.profile = fixture, source, 0, 0, 0
        self.running = False
        self.seed = 1
        self.started_run = 0
    def request(self, command, prefix=None, timeout=3):
        self.fixture.events.append((f"source{self.source}", command))
        fields = command.split()
        if fields[:2] == ["bench", "config"]:
            run = int(fields[2])
            if self.fixture.run != run:
                self.fixture.run = run; self.fixture.raw = []
                self.fixture.states = {1: bytes(54), 2: bytes(54)}
                self.fixture.keyboard = Stream(previous=bytes(34))
            self.seed = int(fields[4]); self.profile = int(fields[7])
        elif command == "bench start":
            self.count = self.crc = 0; self.started_run = self.fixture.run
            self.running = True
            for n in range(1, 11):
                self.fixture.emit(self.source, n); self.count += 1
                self.crc = zlib.crc32(pattern(self.source, self.fixture.run, n, self.seed, self.profile), self.crc)
        elif command == "bench stop":
            self.running = False
            self.fixture.emit(self.source, 0)
        elif command == "bench stats":
            return [f"KMSGEN STATS {self.fixture.run} {self.source} {self.count} {self.count} {self.count} 0 {self.crc:08x} {int(self.running)} 0"]
        elif command == "bench leds":
            return [f"KMSGEN LEDS {getattr(self.fixture, 'led_mask', 0)}"]
        return [prefix] if prefix else []


class Master:
    error = None
    def __init__(self, fixture=None): self.fixture = fixture
    def request(self, command, prefix=None, timeout=3):
        if self.fixture: self.fixture.events.append(("master", command))
        return [prefix] if prefix else []


class Collector:
    error = None
    def __init__(self, fixture): self.fixture = fixture
    def feature(self, *_): pass
    def leds(self, slot, mask): self.fixture.led_mask = mask
    def traces(self, slot): return self.fixture.traces(True)


class RunnerIntegrationTests(unittest.TestCase):
    def execute(self, directory, lose=False, seconds=1, deadline=None, stop_on_failure=True,
                guard_overrun=False, fault_phase=None, trace_fault=False, overload=False,
                profile="everyday", frozen_overlap=False, task="", rate=10,
                fault_event="acknowledgement timeout", injections=(0, 0), rates=None, master_fault_code=None):
        config = Config(master=Device("112233445566", uart="master"),
                        generators=[Device("AABBCCDDEE01", uart="a"), Device("AABBCCDDEE02", uart="b")],
                        receivers=[Device("AABBCCDDEE06", slot=6)], output=directory, stop_on_failure=stop_on_failure)
        if task == "clipboard": config.receivers.append(Device("AABBCCDDEE02", slot=2))
        if rates is not None: config.rates = rates
        runner = Runner(config)
        fixture = Fixture(runner, lose)
        runner.master, runner.generators, runner.capture = Master(fixture), fixture.sources, Collector(fixture)
        self.events = fixture.events
        runner.master_verified = True
        runner.evidence = Evidence(directory, config)
        def fault():
            runner.evidence.log("uart", {"port": "master", "line":
                f"W (100) diagnostics: Slot 6: {fault_event} (detail=0, idle=0ms)"})
        request = runner.master.request
        def master_request(command, prefix=None, timeout=3):
            if fault_phase == "selection" and command == "select 6": fault()
            return request(command, prefix, timeout)
        runner.master.request = master_request
        check = runner.check
        fault_pending = fault_phase == "input"
        def check_input():
            nonlocal fault_pending
            check()
            if fault_pending and runner.segment_open:
                fault(); fault_pending = False
        runner.check = check_input
        def traces(receiver=False):
            trace = fixture.traces(receiver)
            trace["summary"]["injections"] = injections[receiver]
            if frozen_overlap and not receiver:
                trace["summary"]["raw"] = len(trace["records"])
                trace["records"] = trace["records"][:1]
            if trace_fault and receiver:
                trace["records"] = [*trace["records"],
                    {"stage": 12, "a": 7, "b": 6, "c": 0, "micros": 100}]
                trace["summary"]["faults"] = 1
            if master_fault_code is not None and not receiver:
                trace["records"] = [*trace["records"],
                    {"stage": 12, "a": master_fault_code, "b": 6, "c": 0, "micros": 100}]
                trace["summary"]["faults"] = 1
            return trace
        runner.capture.traces = lambda _: traces(True)
        if guard_overrun:
            runner.evidence.log("capture-warning", {"warning": "desktop_event_queue_overrun", "device": "event1"})
        runner.timed_deadline = deadline
        with patch.object(runner, "wait", side_effect=lambda value: self.events.append(("wait", value))), \
             patch("kms_stress.runner.master_traces", side_effect=lambda _: traces()), \
             patch("kms_stress.runner.time.monotonic", new=Clock()):
            result = runner.segment(1, 6, rate, seconds, profile, overload=overload, task=task)
        runner.evidence.finish({"status": result["status"], "segments": [result]})
        return result

    def test_clipboard_load_runs_during_verified_input_and_cannot_hide_failure(self):
        from unittest.mock import Mock
        for passed in (True, False):
            load = Mock()
            load.tick.side_effect = lambda: self.events.append(("clipboard", "tick"))
            load.finish.return_value = {"passed": passed, "complete": passed, "cancelled": passed}
            with tempfile.TemporaryDirectory() as directory, \
                 patch("kms_stress.clipboard.ClipboardTraffic", return_value=load):
                result = self.execute(directory, seconds=3, task="clipboard")
            self.assertGreater(load.tick.call_count, 0)
            self.assertLess(self.events.index(("source1", "bench start")), self.events.index(("clipboard", "tick")))
            self.assertLess(self.events.index(("clipboard", "tick")), self.events.index(("source1", "bench stop")))
            self.assertEqual(result["clipboard"]["passed"], passed)
            self.assertEqual(result["status"] == "FAIL", not passed)
            load.close.assert_called_once()

    def test_background_load_exercises_application_logging_during_input(self):
        with tempfile.TemporaryDirectory() as directory, patch("kms_stress.runner.inspect_cache",
                return_value={"cache_status": 0, "update_guard_clear": True}) as inspect, \
             patch("kms_stress.runner.inspect_radio", return_value={"supported": True, "boot": "1", "tx_failed": 0}) as radio:
            result = self.execute(directory, seconds=3, task="background")
        self.assertEqual(result["errors"], [])
        self.assertIn(("master", "status"), self.events)
        self.assertIn(("master", "slave-status 6"), self.events)
        self.assertLess(self.events.index(("source1", "bench start")), self.events.index(("master", "status")))
        self.assertLess(self.events.index(("master", "status")), self.events.index(("source1", "bench stop")))
        self.assertGreater(inspect.call_count, 0)
        self.assertEqual(result["management_inspection"]["checks"], inspect.call_count)
        self.assertTrue(result["management_inspection"]["update_guard_clear"])
        self.assertEqual(radio.call_count, 2)
        self.assertEqual(result["receiver_radio"]["after"]["tx_failed"], 0)

    def test_background_does_not_hide_receiver_radio_delivery_failures(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch("kms_stress.runner.inspect_cache", return_value={"update_guard_clear": True}), \
             patch("kms_stress.runner.inspect_radio", side_effect=[
                 {"supported": True, "boot": "1", "tx_failed": 0},
                 {"supported": True, "boot": "1", "tx_failed": 4}]):
            result = self.execute(directory, seconds=3, task="background")
        self.assertEqual(result["status"], "FAIL")
        self.assertTrue(any("4 failed radio completion" in error for error in result["errors"]))

    def test_total_deadline_stops_a_long_segment_and_records_actual_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, seconds=60, deadline=101.2, rate=1)
        self.assertTrue(result["duration_limited"])
        self.assertTrue(result["partial"])
        self.assertLess(result["elapsed_seconds"], 5)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertEqual(result["errors"], ["Run deadline ended input before the planned case duration"])
        self.assertTrue(all(not item["running"] for item in result["generators"].values()))

    def test_cleanup_crossing_deadline_does_not_invalidate_completed_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, seconds=1, deadline=101.55, rate=1)
        self.assertEqual(result["status"], "PASS")
        self.assertFalse(result.get("duration_limited"))

    def test_complete_segment_passes_and_writes_real_report(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory)
            # The fixture emits ten reports in a burst. Its simulated elapsed
            # UART/loop time exceeds one second, so delivery verifies while
            # the accurately measured achieved rate is below the request.
            self.assertEqual(result["errors"], []); self.assertEqual(result["status"], "LIMITED")
            self.assertEqual(len(list(Path(directory).glob("*/trace-00001.json"))), 1)

    def test_mixed_soak_has_a_quiet_window_and_independent_generation_rates(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, task="mixed-soak", profile="mixed", rates=[2, 5], rate=5)
        self.assertEqual(result["requested_reports_per_second"], {1: 2, 2: 5})
        configured = [command.split()[3:] for _, command in self.events
                      if isinstance(command, str) and command.startswith("bench config")]
        self.assertEqual(configured, [["1", "1", "2", "6", "5"], ["2", "1", "5", "6", "5"]])
        idle = self.events.index(("wait", 30))
        for source in ("source1", "source2"):
            self.assertLess(idle, self.events.index((source, "bench start")))
            self.assertEqual(self.events.count((source, "bench start")), 1)
        self.assertEqual(result["status"], "PASS")

    def assert_frozen_before_stopping_sources(self):
        frozen = self.events.index(("master", "bench freeze"))
        stopped = self.events.index(("source1", "bench stop"))
        self.assertLess(frozen, stopped)

    def test_continue_after_mismatch_freezes_evidence_without_ending_the_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, lose=True, stop_on_failure=False)
        self.assert_frozen_before_stopping_sources()
        self.assertGreaterEqual(result["elapsed_seconds"], 1)
        self.assertTrue(all(s["completed"] == 10 for s in result["generators"].values()))
        self.assertEqual(result["status"], "INCONCLUSIVE")

    def test_device_fault_freezes_before_stop_even_when_streams_match(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="input", stop_on_failure=False)
        self.assert_frozen_before_stopping_sources()
        self.assertGreaterEqual(result["elapsed_seconds"], 1)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("Device fault: acknowledgement timeout", result["errors"])
        self.assertTrue(all(item["completed"] == 10 for item in result["generators"].values()))

    def test_device_trace_fault_alone_cannot_pass_matching_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, trace_fault=True)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("Device fault: slave_6: USB stall", result["errors"])

    def test_selection_fault_is_retained_without_freezing_new_trace_early(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="selection", stop_on_failure=False)
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["primary_failure"], "acknowledgement timeout")
        self.assertGreater(self.events.index(("master", "bench freeze")),
                           self.events.index(("source1", "bench stop")))

    def test_stop_on_device_fault_does_not_require_a_stream_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="input", seconds=60)
        self.assertEqual(result["status"], "FAIL")
        self.assertLess(result["elapsed_seconds"], 5)
        self.assert_frozen_before_stopping_sources()

    def test_explicit_overload_records_fault_and_requires_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="input", overload=True,
                                  fault_event="transmit queue overflow", master_fault_code=5)
        self.assertEqual(result["status"], "OVERLOAD OBSERVED")
        self.assertTrue(result["requires_recovery_check"])

    def test_low_load_usb_stall_and_ack_timeout_are_failures_even_in_overload_preset(self):
        for event in ("USB stall", "acknowledgement timeout"):
            with self.subTest(event=event), tempfile.TemporaryDirectory() as directory:
                result = self.execute(directory, fault_phase="input", overload=True,
                                      fault_event=event, rate=1)
            self.assertEqual(result["status"], "FAIL")
            self.assertNotIn("requires_recovery_check", result)

    def test_overload_pressure_does_not_excuse_an_additional_unrelated_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="input", overload=True,
                                  fault_event="transmit queue overflow", master_fault_code=5, trace_fault=True)
        self.assertEqual(result["status"], "FAIL")

    def test_overload_requires_saturation_in_the_armed_window(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, fault_phase="selection", overload=True,
                                  fault_event="transmit queue overflow")
        self.assertEqual(result["status"], "FAIL")

    def test_resilience_requires_observed_drops_on_both_links(self):
        for injections in ((0, 0), (2, 0), (0, 2), (2, 2)):
            with self.subTest(injections=injections), tempfile.TemporaryDirectory() as directory, \
                 patch.object(Runner, "availability_check", return_value={"passed": True}):
                result = self.execute(directory, task="resilience", rate=1, injections=injections)
            self.assertEqual(result["injections"], dict(zip(("input", "ack"), injections)))
            self.assertEqual(result["status"], "PASS" if all(injections) else "INCONCLUSIVE")

    def test_frozen_overlap_reference_is_inconclusive_without_a_proven_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, profile="overlap", frozen_overlap=True)
        self.assertEqual(result["status"], "INCONCLUSIVE")
        self.assertTrue(result["overlap_reference_incomplete"])
        self.assertFalse(any("merge/release differed" in e for e in result["errors"]))

    def test_proven_fault_still_fails_when_overlap_reference_is_frozen(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, profile="overlap", frozen_overlap=True, fault_phase="input")
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("Device fault: acknowledgement timeout", result["errors"])

    def test_lost_linux_frame_is_inconclusive_and_localized(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, True)
            self.assertEqual(result["status"], "INCONCLUSIVE")
            self.assertEqual(result["first_divergence"], "Linux HID capture path")

    def test_guard_warning_does_not_mask_a_lost_hid_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, lose=True, guard_overrun=True)
            self.assertEqual(result["status"], "INCONCLUSIVE")
            self.assertEqual(result["first_divergence"], "Linux HID capture path")

    def test_verified_hid_stream_can_pass_despite_guard_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute(directory, guard_overrun=True)
            self.assertEqual(result["errors"], [])
            self.assertEqual(result["status"], "LIMITED")

    def test_quiet_receivers_cannot_pass_continuous_handover(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Config(receivers=[Device("AABBCCDDEE03", slot=3), Device("AABBCCDDEE06", slot=6)], output=directory)
            runner = Runner(config)
            fixture = Fixture(runner)
            fixture.emit = lambda *_: None  # Sources complete, but Linux receives nothing.
            runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
            runner.master_verified = True
            runner.evidence = Evidence(directory, config)
            with patch.object(runner, "wait"), patch("kms_stress.runner.master_traces", side_effect=lambda _: fixture.traces()), \
                 patch("kms_stress.runner.time.monotonic", new=Clock()):
                result = runner.live_handover(1)
            runner.evidence.finish({"status": result["status"], "segments": [result]})
            self.assertEqual(result["status"], "FAIL")
            self.assertTrue(any("no active input observed" in error for error in result["errors"]))

    def test_live_handover_requires_complete_fault_free_observations(self):
        for condition, status in ((None, "PASS"), ("uart", "FAIL"), ("trace", "FAIL"), ("deadline", "INCONCLUSIVE")):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as directory:
                config = Config(master=Device("112233445566", uart="master"),
                                receivers=[Device("AABBCCDDEE03", slot=3), Device("AABBCCDDEE06", slot=6)],
                                output=directory, stop_on_failure=False)
                runner = Runner(config)
                fixture = Fixture(runner)
                fixture.emit = lambda *_: None
                runner.master, runner.generators, runner.capture = Master(), fixture.sources, Collector(fixture)
                runner.master_verified = True
                runner.evidence = Evidence(directory, config)
                clock = Clock()
                visits = []
                def select(slot):
                    visits.append(slot)
                    if condition == "deadline" and len(visits) == 4:
                        runner.timed_deadline = clock.now
                def deliver():
                    if runner.segment_open:
                        runner.consume(runner.active, 0, b"\x02" + bytes(33), 0)
                        runner.consume(runner.active, 0, bytes(34), 0)
                        if condition == "uart" and not runner.evidence.faults:
                            runner.evidence.log("uart", {"port": "master", "line":
                                "W (100) diagnostics: Slot 6: acknowledgement timeout (detail=0)"})
                def traces():
                    trace = fixture.traces()
                    if condition == "trace":
                        trace["records"] = [{"stage": 12, "a": 7, "b": 6, "c": 0, "micros": 100}]
                    return trace
                with patch.object(runner, "wait"), patch.object(runner, "check", side_effect=deliver), \
                     patch.object(runner, "select_receiver", side_effect=select), \
                     patch.object(runner.cancel, "wait"), patch("kms_stress.runner.master_traces", side_effect=lambda _: traces()), \
                     patch("kms_stress.runner.time.monotonic", new=clock):
                    result = runner.live_handover(1)
                runner.evidence.finish({"status": result["status"], "segments": [result]})
                self.assertEqual(result["status"], status)
                if condition == "deadline":
                    self.assertEqual(result["transitions"], [(3, 6)])
                    self.assertTrue(result["duration_limited"])
                else:
                    self.assertEqual(result["transitions"], [(3, 6), (6, 3)])
                if status == "FAIL":
                    self.assertTrue(any("Device fault:" in error for error in result["errors"]))


if __name__ == "__main__": unittest.main()
