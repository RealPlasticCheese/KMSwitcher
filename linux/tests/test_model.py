import copy
import ctypes
import json
import os
from pathlib import Path
import random
import re
import struct
import tempfile
import unittest
import zlib

from kms_stress.model import (Config, Device, MAX_MIXED_SOAK_SECONDS, MIXED_PROFILE, PROFILES, OverlapVerifier,
                              Stream, Verifier, handovers, pattern, trace_page, verify_overlap)
from kms_stress.runner import Evidence, Runner, demo, parse_stats, serial_hash, verify_inventory


def configuration():
    return Config(master=Device("112233445566", uart="/dev/serial/by-id/master"),
                  generators=[Device("AABBCCDDEE01", uart="/dev/serial/by-id/source-a"),
                              Device("AABBCCDDEE02", uart="/dev/serial/by-id/source-b")],
                  receivers=[Device(f"AABBCCDDEE{slot + 10:02X}", slot=slot) for slot in range(1, 7)])


def simulate(profile=1, count=100, sources=(1, 2), seed=1, omit=None, duplicate=None, corrupt=None):
    verifier = Verifier(sources, profile, seed)
    states = {s: pattern(s, 42, 0, seed, profile) for s in sources}
    previous = bytes(34)
    for sequence in range(1, count + 1):
        for source in sources:
            data = pattern(source, 42, sequence, seed, profile); states[source] = data
            keys = mods = buttons = 0
            consumers = set()
            for state in states.values():
                keys |= int.from_bytes(state[1:33], "little"); mods |= state[0]
                buttons |= int.from_bytes(state[33:35], "little")
                usage = int.from_bytes(state[41:43], "little")
                if usage: consumers.add(usage)
            keyboard = bytes([mods, 0]) + keys.to_bytes(32, "little")
            if keyboard != previous:
                if omit != (source, sequence, 0): verifier.feed(0, keyboard)
                if duplicate == (source, sequence, 0): verifier.feed(0, keyboard)
                previous = keyboard
            mouse = struct.pack("<H", buttons) + data[35:41]
            if corrupt == (source, sequence, 1):
                mouse = mouse[:2] + bytes([mouse[2] ^ 1]) + mouse[3:]
            if omit != (source, sequence, 1): verifier.feed(1, mouse)
            if duplicate == (source, sequence, 1): verifier.feed(1, mouse)
            controls = b"\x01" + struct.pack("<4H", *sorted(consumers), *([0] * (4 - len(consumers))))
            verifier.feed(2, controls)
    verifier.feed(0, bytes(34)); verifier.feed(1, bytes(8)); verifier.feed(2, b"\x01" + bytes(8))
    return verifier.finish(42, seed, {s: count for s in sources})


class ConfigTests(unittest.TestCase):
    def test_six_receivers_two_sources(self): configuration().validate()

    def test_noncontiguous_subset(self):
        c = configuration(); c.receivers = [c.receivers[i] for i in (2, 3, 5)]; c.generators.pop(); c.validate()

    def test_seventh_receiver_supported(self):
        c = configuration(); c.receivers.append(Device("AABBCCDDEEFF", slot=7)); c.validate()

    def test_mixed_soak_uses_two_sources_and_respects_the_generator_duration(self):
        c = configuration()
        c.preset, c.rates, c.soak_seconds, c.campaign_idle_seconds = "mixed-soak", [200, 10], MAX_MIXED_SOAK_SECONDS, 180
        c.validate()
        self.assertEqual(c.workload()["rates_per_generator"], [10, 200])
        self.assertEqual(c.workload()["combined_rates"], [210])
        self.assertEqual(list(Runner(c).schedule()),
                         [(slot, 200, MAX_MIXED_SOAK_SECONDS, "mixed", False, "mixed-soak") for slot in range(1, 7)])
        self.assertEqual(Runner(c).planned_seconds(), 6 * (180 + MAX_MIXED_SOAK_SECONDS))
        c.soak_seconds += 1
        with self.assertRaisesRegex(ValueError, "duration per receiver"):
            c.validate()
        c.soak_seconds = 60
        c.generators.pop()
        with self.assertRaisesRegex(ValueError, "requires two"):
            c.validate()

    def test_disabled_absent_receivers(self):
        c = configuration()
        for d in c.receivers[:2]: d.enabled = False
        c.validate()

    def test_invalid_counts(self):
        for role, count in (("generators", 0), ("generators", 3), ("receivers", 0), ("receivers", 8)):
            with self.subTest(role=role, count=count):
                c = configuration(); setattr(c, role, [Device(f"ABCDEF0000{i:02X}", slot=i, uart=str(i)) for i in range(count)])
                with self.assertRaises(ValueError): c.validate()

    def test_duplicate_identity(self):
        c = configuration(); c.receivers[0].identity = c.generators[0].identity
        with self.assertRaises(ValueError): c.validate()

    def test_master_cannot_share_generator_identity(self):
        c = configuration(); c.master.identity = c.generators[0].identity
        with self.assertRaises(ValueError): c.validate()

    def test_duplicate_slot(self):
        c = configuration(); c.receivers[1].slot = c.receivers[0].slot
        with self.assertRaises(ValueError): c.validate()

    def test_duplicate_uart(self):
        c = configuration(); c.generators[0].uart = c.master.uart
        with self.assertRaises(ValueError): c.validate()

    def test_bad_identity_not_assigned_implicitly(self):
        c = configuration(); c.receivers[0].identity = ""
        with self.assertRaises(ValueError): c.validate()

    def test_bounds(self):
        for key, value in (("seconds", 0), ("rates", [1001]), ("seed", -1), ("max_log_mb", 1), ("soak_seconds", 0)):
            with self.subTest(key=key):
                c = configuration(); setattr(c, key, value)
                with self.assertRaises(ValueError): c.validate()

    def test_roundtrip_keeps_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"; c = configuration(); c.save(path)
            self.assertEqual(Config.load(path), c)

    def test_profile_version(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"; path.write_text('{"version":2}')
            with self.assertRaises(ValueError): Config.load(path)

    def test_routes_cover_thirty_directed_pairs(self):
        pairs = handovers(range(1, 7)); self.assertEqual(len(set(pairs)), 30)
        self.assertNotIn((1, 1), pairs)


class OracleTests(unittest.TestCase):
    def test_normal_and_stress_two_sources(self):
        for profile in (0, 1, 2, 4):
            for seed in (0, 1, 0xffffffff):
                with self.subTest(profile=profile, seed=seed): self.assertEqual(simulate(profile, seed=seed)["errors"], [])

    def test_one_source(self): self.assertEqual(simulate(sources=(1,))["errors"], [])

    def test_mixed_soak_typing_bursts_and_continuous_pointer(self):
        for sequence, pressed in ((1, True), (2, False), (15, True), (17, False), (63, False), (65, True)):
            keys = pattern(1, 42, sequence, 1, MIXED_PROFILE)
            mouse = pattern(2, 42, sequence, 1, MIXED_PROFILE)
            self.assertEqual(bool(any(keys[:33])), pressed)
            self.assertEqual(keys[33:43], bytes(10))
            self.assertEqual(mouse[:33], bytes(33))
            self.assertTrue(any(mouse[33:41]))
        self.assertEqual(simulate(MIXED_PROFILE)["errors"], [])
        self.assertTrue(simulate(MIXED_PROFILE, omit=(1, 1, 0))["errors"])
        self.assertTrue(simulate(MIXED_PROFILE, omit=(2, 25, 1))["errors"])

    def test_inactive_mouse_axis_and_scroll_are_rejected_in_both_directions(self):
        for source, axes in ((1, (0, 123, 0, 7)), (2, (123, 0, 7, 0))):
            for profile in (0, 2):
                with self.subTest(source=source, profile=profile):
                    v = Verifier([source], profile)
                    v.feed(1, struct.pack("<Hhhbb", 0, *axes))
                    self.assertEqual(v.errors, ["Unexpected movement or scroll on an inactive source's axis"])

    def test_mouse_loss_detected(self): self.assertTrue(simulate(omit=(1, 25, 1))["errors"])

    def test_mouse_corruption_detected(self): self.assertTrue(simulate(corrupt=(1, 25, 1))["errors"])

    def test_duplicate_mouse_detected(self): self.assertTrue(simulate(duplicate=(2, 25, 1))["errors"])

    def test_duplicate_keyboard_detected(self): self.assertTrue(simulate(duplicate=(1, 25, 0))["errors"])

    def test_missing_release_detected(self):
        v = Verifier([1], 0); data = pattern(1, 42, 1, 1, 0)
        v.feed(0, data[:1] + b"\0" + data[1:33]); v.feed(1, data[33:41])
        self.assertTrue(v.finish(42, 1, {1: 1})["errors"])

    def test_unknown_keyboard_and_control_rejected(self):
        v = Verifier([1], 0)
        v.feed(0, b"\0\0" + (1 << 100).to_bytes(32, "little")); v.feed(2, b"\x02\x01")
        self.assertEqual(len(v.errors), 2)

    def test_bad_lengths(self):
        v = Verifier([1], 0)
        for interface, data in ((0, bytes(8)), (1, bytes(3)), (2, bytes(2)), (3, bytes(8))): v.feed(interface, data)
        self.assertEqual(len(v.errors), 4)

    def test_neutral_duplicates_allowed(self):
        v = Verifier([1], 0)
        for _ in range(5): v.feed(0, bytes(34))
        self.assertEqual(v.keys[1].count, 0)

    def test_pointer_coalescence_same_state(self):
        # Sequence 1–3 has no held buttons and positive X, so combining it is legal.
        v = Verifier([1], 2)
        records = [pattern(1, 42, n, 1, 2) for n in range(1, 4)]
        x = sum(struct.unpack_from("<h", p, 35)[0] for p in records)
        wheel = sum(struct.unpack_from("<b", p, 39)[0] for p in records)
        v.feed(1, struct.pack("<Hhhbb", 0, x, 0, wheel, 0))
        self.assertEqual(v.finish(42, 1, {1: 3})["errors"], [])

    def test_zero_release_has_metadata_but_no_input(self):
        data = pattern(2, 1234, 0, 99, 1)
        self.assertEqual(data[:43], bytes(43)); self.assertEqual(data[43:46], b"KM\x02")


class ProtocolTests(unittest.TestCase):
    def page(self, index=0, count=1, frozen=True):
        page = bytearray(128)
        struct.pack_into("<4s6I", page, 0, b"KMT1", 123, 42, count, 0, 3 if frozen else 1, index)
        if index < count:
            struct.pack_into("<7I", page, 28, index, 100, 1, 2, 3, 4, 3); page[56:59] = b"abc"
        struct.pack_into("<I", page, 124, zlib.crc32(page[:124])); return bytes(page)

    def test_page_decode(self): self.assertEqual(trace_page(self.page())["record"]["data"], "616263")

    def test_crc_rejected(self):
        p = bytearray(self.page()); p[10] ^= 1
        with self.assertRaises(ValueError): trace_page(p)

    def test_unfrozen_page_rejected(self):
        with self.assertRaises(ValueError): trace_page(self.page(frozen=False))

    def test_stale_record_rejected(self):
        p = bytearray(self.page()); p[28] = 2; struct.pack_into("<I", p, 124, zlib.crc32(p[:124]))
        with self.assertRaises(ValueError): trace_page(p)

    def test_stats_identity(self):
        stats = parse_stats("KMSGEN STATS 42 2 100 99 99 1 1234abcd 0 0", 42, 2)
        self.assertEqual(stats["completed"], 99); self.assertEqual(stats["crc"], 0x1234abcd)
        with self.assertRaises(ValueError): parse_stats("KMSGEN STATS 41 2 100 99 99 1 1234abcd 0 0", 42, 2)

    def test_inventory_checks_source_serial_not_only_vid(self):
        c = configuration(); lines = []
        for address, d in enumerate(c.generators, 2):
            lines += [f"KMSUSB DEVICE {address} 303a 4006 {serial_hash(d.identity):08x} 1 1 1 00",
                      f"KMSUSB INTERFACE {address} 0 03 1 3 1 active 100 0 0"]
        verify_inventory(lines, c.generators)
        with self.assertRaises(RuntimeError): verify_inventory(lines, [Device("112233445566")])

    def test_shared_key_release_uses_actual_arrival_order(self):
        v = OverlapVerifier([1, 2]); records = []
        for source, sequence in ((1, 1), (2, 1), (1, 2), (2, 2)):
            data = pattern(source, 42, sequence, 1, 3)
            records.append({"stage": 1, "data": (b"\x01" + data).hex()})
        on = bytes(2) + (1 << 4).to_bytes(32, "little")
        v.feed(0, on); v.feed(0, bytes(34))
        result = v.finish(42, 1, {1: 2, 2: 2}); result["run"] = 42
        verify_overlap(result, {"overwritten": 0, "records": records}, {1: 2, 2: 2})
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["overlap_evidence"], {"key": True, "button": False})
        result["actual_keyboard"]["count"] += 1
        verify_overlap(result, {"overwritten": 0, "records": records}, {1: 2, 2: 2})
        self.assertTrue(result["errors"])

    def test_overlap_evidence_distinguishes_sequential_and_simultaneous_key_and_button_holds(self):
        key_down = bytes(2) + (1 << 4).to_bytes(32, "little")
        for concurrent in (False, True):
            with self.subTest(concurrent=concurrent):
                verifier = OverlapVerifier([1, 2])
                arrivals = [(source, sequence) for source in (1, 2) for sequence in (1, 2, 3, 4, 0)]
                if concurrent:
                    arrivals.sort(key=lambda item: item[1] or 5)
                records = [{"stage": 1, "data": (b"\x01" + pattern(source, 42, sequence, 1, 3)).hex()}
                           for source, sequence in arrivals]
                for _ in range(1 if concurrent else 2):
                    for state in (key_down, bytes(34), key_down, bytes(34)):
                        verifier.feed(0, state)
                    verifier.feed(1, b"\x01" + bytes(7))
                    verifier.feed(1, bytes(8))
                result = verifier.finish(42, 1, {1: 4, 2: 4})
                result["run"] = 42
                verify_overlap(result, {"overwritten": 0, "records": records}, {1: 4, 2: 4})
                self.assertEqual(result["errors"], [])
                self.assertEqual(result["overlap_evidence"], {"key": concurrent, "button": concurrent})


class ReportAndScheduleTests(unittest.TestCase):
    def test_demo_all_configured_receivers(self):
        with tempfile.TemporaryDirectory() as directory:
            c = configuration(); c.output = directory
            result, path = demo(c)
            self.assertEqual(result["status"], "DEMO PASS"); self.assertEqual(len(result["segments"]), 6)
            self.assertFalse(result["hardware_test"]); self.assertTrue((path / "report.html").is_file())

    def test_disk_budget_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as directory:
            c = configuration(); evidence = Evidence(directory, c); evidence.limit = 1
            with self.assertRaises(RuntimeError): evidence.log("test", {"x": "data"})
            evidence.finish({"status": "INCONCLUSIVE"})

    def test_updating_suite_progress_charges_only_the_retained_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, configuration())
            evidence.save("suite-progress.json", {"cycle": 1})
            original = evidence.size
            evidence.limit = original
            for cycle in range(2, 9):
                evidence.save("suite-progress.json", {"cycle": cycle})
            self.assertEqual(evidence.size, original)
            with self.assertRaisesRegex(RuntimeError, "budget"):
                evidence.save("suite-progress.json", {"cycle": 10})
            self.assertEqual(json.loads((evidence.path / "suite-progress.json").read_text()), {"cycle": 8})
            evidence.finish({"status": "PASS"})

    def test_traces_share_budget_and_final_report_survives_exhaustion(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, configuration())
            evidence.limit = evidence.size + 100
            evidence.save("trace-first.json", {"events": "x" * 50})
            with self.assertRaisesRegex(RuntimeError, "budget exceeded"):
                evidence.save("trace-second.json", {"events": "x" * 50})
            evidence.save("exception.json", {"error": "budget exceeded"}, required=True)
            evidence.finish({"status": "INCONCLUSIVE"})
            self.assertTrue((evidence.path / "report.html").is_file())
            self.assertFalse((evidence.path / "trace-second.json").exists())

    def test_html_escapes_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, configuration()); evidence.finish({"status": "FAIL", "message": "<script>x</script>"})
            page = (evidence.path / "report.html").read_text()
            self.assertNotIn("<script>", page); self.assertIn("&lt;script&gt;", page)

    def test_desktop_guard_overruns_are_counted_in_json_and_html(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, configuration())
            for device in ("event1", "event2"):
                evidence.log("capture-warning", {"warning": "desktop_event_queue_overrun", "device": device})
            evidence.finish({"status": "PASS"})
            result = json.loads((evidence.path / "result.json").read_text())
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(result["capture_warnings"], {"desktop_event_queue_overrun": 2})
            self.assertIn("Desktop event guard queue overruns: 2", (evidence.path / "report.html").read_text())

    def test_routing_schedule_covers_all_pairs(self):
        c = configuration(); c.preset = "routing"
        targets = [item[0] for item in Runner(c).schedule()]
        self.assertTrue(set(handovers(range(1, 7))).issubset(set(zip(targets, targets[1:]))))

    def test_overload_followed_by_recovery(self):
        c = configuration(); c.preset = "overload"; schedule = list(Runner(c).schedule())
        self.assertEqual(len(schedule), 12)
        self.assertTrue(schedule[0][4]); self.assertFalse(schedule[1][4]); self.assertEqual(schedule[0][0], schedule[1][0])

    def test_overlap_uses_bounded_complete_trace(self):
        c = configuration(); c.preset = "overlap"
        self.assertTrue(all(item[1:4] == (2, 2, "overlap") for item in Runner(c).schedule()))


@unittest.skipUnless(os.environ.get("KMS_STRESS_PATTERN_DLL"), "Native DLL is built by scripts/test-stress.py")
class NativePatternTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = ctypes.CDLL(os.environ["KMS_STRESS_PATTERN_DLL"])
        cls.native.kms_generator_pattern.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint32,
                                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint]
        root = Path(__file__).resolve().parents[2]
        text = (root / "diagnostics/hid-generator/main/main.c").read_text()
        body = re.search(r"static const uint8_t descriptor\[\] = \{(.*?)\};", text, re.S).group(1)
        cls.descriptor = bytes(int(n, 0) for n in re.findall(r"0x[0-9a-f]+|\b[0-9]+\b", body))

    def test_c_and_independent_python_patterns_match(self):
        rng = random.Random(3812)
        out = ctypes.create_string_buffer(54)
        for _ in range(10000):
            source, run, sequence, seed, profile = (rng.choice((1, 2)), rng.getrandbits(32), rng.getrandbits(32),
                                                    rng.getrandbits(32), rng.choice(tuple(PROFILES.values())))
            self.native.kms_generator_pattern(out, source, run, sequence, seed, profile)
            self.assertEqual(out.raw, pattern(source, run, sequence, seed, profile))

    def test_real_generator_descriptor_decodes_with_production_parser(self):
        descriptor = ctypes.create_string_buffer(self.descriptor)
        for source in (1, 2):
            for profile in PROFILES.values():
                for sequence in (0, 1, 2, 31, 32, 255, 256, 65535, 65536, 0xffffffff):
                    result = self.native.kms_generator_decode_test(descriptor, len(self.descriptor), source, ctypes.c_uint32(sequence), ctypes.c_uint32(0xffffffff), profile)
                    self.assertEqual(result, 0, (source, profile, sequence, result))

    def trace_command(self, op, value=0):
        if op == 1: self.native.kms_test_mode_set(True)
        data = ctypes.create_string_buffer(struct.pack("<4sII", b"KMT1", op, value))
        self.native.kms_test_command(data, 12)

    def trace_record(self, stage, data, a=0):
        buffer = ctypes.create_string_buffer(data)
        self.native.kms_test_record(stage, a, 0, 0, buffer, len(data))

    def summary(self):
        self.trace_command(2); self.trace_command(4, 0xffffffff)
        out = ctypes.create_string_buffer(128)
        self.assertEqual(self.native.kms_test_page(out, 128), 128)
        return trace_page(out.raw)["summary"]

    def test_full_stream_summary_matches_independent_input(self):
        self.native.kms_test_init(123, True); self.trace_command(1, 42)
        keys, crc, payload_crc, count = 0, 0, 0, 0
        states = {1: bytes(54), 2: bytes(54)}
        previous = bytes(34)
        for sequence in range(1, 101):
            for source in (1, 2):
                data = pattern(source, 42, sequence, 1, 1)
                states[source] = data; self.trace_record(1, b"\x01" + data)
                merged = int.from_bytes(states[1][1:33], "little") | int.from_bytes(states[2][1:33], "little")
                keyboard = bytes(2) + merged.to_bytes(32, "little")
                if keyboard != previous:
                    crc = zlib.crc32(keyboard, crc); count += 1; previous = keyboard
                sample = bytearray(60); sample[:32] = merged.to_bytes(32, "little")
                self.trace_record(3, bytes(sample))
                if source == 1: payload_crc = zlib.crc32(data, payload_crc)
        result = self.summary()
        self.assertEqual((result["reference_keys_count"], result["reference_keys_crc"]), (count, crc))
        self.assertEqual((result["routed_keys_count"], result["routed_keys_crc"]), (count, crc))
        self.assertEqual((result["source1_count"], result["source1_crc"]), (100, payload_crc))

    def test_summary_survives_ring_wrap(self):
        self.native.kms_test_init(123, True); self.trace_command(1, 42)
        for sequence in range(1, 1001): self.trace_record(1, b"\x01" + pattern(1, 42, sequence, 1, 1))
        self.assertEqual(self.summary()["source1_count"], 1000)

    def test_wrong_run_does_not_contaminate_reference(self):
        self.native.kms_test_init(123, True); self.trace_command(1, 42)
        self.trace_record(1, b"\x01" + pattern(1, 99, 1, 1, 1))
        self.assertEqual(self.summary()["source1_count"], 0)

    def test_usb_completion_summary(self):
        self.native.kms_test_init(123, False); self.trace_command(1, 42)
        key = bytes(2) + (1 << 4).to_bytes(32, "little")
        self.trace_record(10, key); self.trace_record(10, bytes(34))
        self.trace_record(10, bytes(34))
        result = self.summary()
        self.assertEqual(result["completed_keys_count"], 2)
        self.assertEqual(result["completed_keys_crc"], zlib.crc32(bytes(34), zlib.crc32(key)))

    def test_cross_source_order_difference_is_detectable(self):
        self.native.kms_test_init(123, True); self.trace_command(1, 42)
        for source in (1, 2): self.trace_record(1, b"\x01" + pattern(source, 42, 1, 1, 1))
        sample = bytearray(60); sample[:32] = ((1 << 4) | (1 << 20)).to_bytes(32, "little")
        self.trace_record(3, bytes(sample))
        result = self.summary()
        self.assertNotEqual(result["reference_keys_count"], result["routed_keys_count"])


if __name__ == "__main__": unittest.main()
