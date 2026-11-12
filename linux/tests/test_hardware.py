"""Adapter/lifecycle tests with fake UART, Linux descriptors and ioctl calls."""
import json
from pathlib import Path
import queue
import struct
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import Mock, patch

from kms_stress.hardware import Capture, ReceiverNodes, SerialChannel, discover_serial, read_identity
from kms_stress.model import Config, Device
from kms_stress.runner import Runner, parse_stats


class FakeSerial:
    instances = []
    def __init__(self):
        self.data, self.writes = queue.Queue(), []
        self.dtr = self.rts = True
        self.in_waiting = 0
        self.closed = False
        self.instances.append(self)

    def open(self):
        if self.dtr or self.rts: raise AssertionError("Opening UART must not assert reset/boot")

    def read(self, count):
        try: return self.data.get(timeout=0.01)
        except queue.Empty: return b""

    def write(self, data):
        self.writes.append(data)
        if data == b"bench info\n": self.data.put(b"boot log\r\nKMSGEN INFO 1 AABBCCDDEEFF 54 version\r\n")
        elif data == b"bad\n": self.data.put(b"KMSGEN ERROR invalid command\n")

    def close(self): self.closed = True


class UARTTests(unittest.TestCase):
    def open(self):
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=FakeSerial)}):
            return SerialChannel("/dev/serial/by-id/chosen-by-user")

    def test_identity_handshake_and_safe_open(self):
        channel = self.open()
        try:
            self.assertEqual(channel.request("bench info", "KMSGEN INFO")[-1].split()[3], "AABBCCDDEEFF")
            self.assertTrue(channel.port.exclusive); self.assertFalse(channel.port.dtr); self.assertFalse(channel.port.rts)
        finally: channel.close()
        self.assertTrue(channel.port.closed)

    def test_discovery_ignores_boot_and_route_notices(self):
        class MasterSerial(FakeSerial):
            def write(self, data):
                self.writes.append(data)
                if data == b"bench info\n":
                    self.data.put(b"KMSBENCH BOOT 12345678\nKMSROUTE CURRENT 6\n"
                                  b"KMSBENCH INFO 1 master AABBCCDDEEFF 12345678 6\n")
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=MasterSerial)}), \
             patch("kms_stress.hardware.serial_ports", return_value=["/dev/serial/by-id/master"]):
            found, problems = discover_serial()
        self.assertEqual(problems, [])
        self.assertEqual([(role, device.identity) for role, device in found], [("master", "AABBCCDDEEFF")])
        self.assertEqual(MasterSerial.instances[-1].writes, [b"\n", b"bench info\n"])

    def test_identity_recovers_when_opening_uart_loses_first_command(self):
        class StartingSerial(FakeSerial):
            def write(self, data):
                self.writes.append(data)
                if data == b"bench info\n" and self.writes.count(data) > 1:
                    self.data.put(b"\x1b[32mKMSBENCH INFO 1 master AABBCCDDEEFF 12345678 0\x1b[0m\n")
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=StartingSerial)}):
            channel = SerialChannel("bridge")
        try:
            info = read_identity(channel, timeout=0.02)
            self.assertEqual((info.role, info.identity, info.boot), ("master", "AABBCCDDEEFF", "12345678"))
            self.assertEqual(channel.port.writes, [b"\n", b"bench info\n", b"\n", b"bench info\n"])
            self.assertFalse(channel.port.dtr); self.assertFalse(channel.port.rts)
        finally: channel.close()

    def test_silent_uart_has_bounded_read_only_retries(self):
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=FakeSerial)}):
            channel = SerialChannel("bridge")
        try:
            channel.port.write = lambda data: channel.port.writes.append(data)
            with self.assertRaisesRegex(TimeoutError, "no UART data received"):
                read_identity(channel, attempts=2, timeout=0.02)
            self.assertEqual(channel.port.writes, [b"\n", b"bench info\n"] * 2)
        finally: channel.close()

    def test_generator_startup_command_rejection_is_retried_read_only(self):
        class StartingGenerator(FakeSerial):
            def write(self, data):
                self.writes.append(data)
                if data == b"bench info\n":
                    reply = (b"KMSGEN ERROR invalid command\n" if self.writes.count(data) == 1 else
                             b"KMSGEN INFO 1 AABBCCDDEEFF 54 version\n")
                    self.data.put(reply)
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=StartingGenerator)}):
            channel = SerialChannel("starting generator")
        try:
            self.assertEqual(read_identity(channel).identity, "AABBCCDDEEFF")
            self.assertEqual(channel.port.writes, [b"\n", b"bench info\n"] * 2)
        finally: channel.close()

    def test_identity_waits_for_observed_two_second_master_startup(self):
        class RebootingSerial(FakeSerial):
            def open(self):
                super().open()
                self.opened = time.monotonic()

            def write(self, data):
                self.writes.append(data)
                if data == b"bench info\n" and time.monotonic() - self.opened >= 1.95:
                    self.data.put(b"KMSBENCH INFO 1 master AABBCCDDEEFF 12345678 0\n")
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=RebootingSerial)}):
            channel = SerialChannel("rebooting bridge")
        try:
            info = read_identity(channel)
            self.assertEqual(info.role, "master")
            self.assertGreaterEqual(channel.port.writes.count(b"bench info\n"), 2)
            self.assertLessEqual(channel.port.writes.count(b"bench info\n"), 4)
            self.assertFalse(channel.port.dtr); self.assertFalse(channel.port.rts)
        finally: channel.close()

    def test_timeout_retains_unrecognized_firmware_reply(self):
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=FakeSerial)}):
            channel = SerialChannel("bridge")
        try:
            channel.port.write = lambda _: channel.port.data.put(b"Command not recognized or unavailable\n")
            with self.assertRaisesRegex(TimeoutError, "Command not recognized"):
                read_identity(channel, attempts=1, timeout=0.02)
        finally: channel.close()

    def test_malformed_identity_is_not_trusted(self):
        for reply in ("KMSGEN INFO 1 not-a-chip 54 git", "KMSGEN INFO 2 AABBCCDDEEFF 54 git",
                      "KMSBENCH INFO 1 master AABBCCDDEEFF bad-boot 0"):
            with self.subTest(reply=reply):
                channel = Mock(path="bridge")
                channel.request.return_value = [reply]
                with self.assertRaises(ValueError): read_identity(channel)

    def test_error_is_not_treated_as_success(self):
        channel = self.open()
        try:
            with self.assertRaises(RuntimeError): channel.request("bad", "KMSGEN OK")
        finally: channel.close()

    def test_timeout_is_explicit(self):
        channel = self.open()
        try:
            with self.assertRaises(TimeoutError): channel.request("silent", "MISSING", timeout=0.02)
        finally: channel.close()

    def test_no_response_command(self):
        channel = self.open()
        try: self.assertEqual(channel.request("bench ping"), [])
        finally: channel.close()

    def test_inconsistent_stats_rejected(self):
        with self.assertRaises(ValueError): parse_stats("KMSGEN STATS 1 1 10 9 11 1 abc 0 0", 1, 1)

    def test_nonboolean_stats_flag_rejected(self):
        with self.assertRaises(ValueError): parse_stats("KMSGEN STATS 1 1 10 9 9 1 abc 7 0", 1, 1)


class FakeSelector:
    def register(self, *args): pass
    def close(self): pass
    def select(self, timeout):
        time.sleep(min(timeout, 0.01))
        return []


class FakeGuard:
    def __init__(self, path): self.fd, self.path, self.grabbed, self.closed = 1, path, False, False
    def grab(self): self.grabbed = True
    def ungrab(self): self.grabbed = False
    def close(self): self.closed = True


class CaptureTests(unittest.TestCase):
    def setUp(self):
        nonblock = patch("kms_stress.hardware.os.O_NONBLOCK", 0x800, create=True)
        nonblock.start(); self.addCleanup(nonblock.stop)

    def test_grabs_all_nodes_and_closes_every_descriptor(self):
        guards = []
        def guard(path):
            value = FakeGuard(path); guards.append(value); return value
        nodes = {"AABBCCDDEEFF": ReceiverNodes("AABBCCDDEEFF", {0: "keyboard", 1: "mouse", 2: "controls"}, ["event1", "event2", "event3"])}
        with patch.dict(sys.modules, {"evdev": types.SimpleNamespace(InputDevice=guard)}), \
             patch("kms_stress.hardware.receiver_nodes", return_value=nodes), \
             patch("kms_stress.hardware.selectors.DefaultSelector", return_value=FakeSelector()), \
             patch("kms_stress.hardware.os.open", side_effect=(101, 102, 103)), \
             patch("kms_stress.hardware.os.close") as close, patch.object(Capture, "_read", return_value=None):
            capture = Capture([Device("AABBCCDDEEFF", slot=6)], lambda *_: None)
            capture.open(); self.assertTrue(all(g.grabbed for g in guards)); capture.close()
            self.assertEqual(close.call_count, 3); self.assertTrue(all(g.closed and not g.grabbed for g in guards))

    def test_partial_open_failure_releases_prior_grab(self):
        guards = []
        def guard(path):
            value = FakeGuard(path); guards.append(value); return value
        nodes = {"AABBCCDDEEFF": ReceiverNodes("AABBCCDDEEFF", {0: "k", 1: "m", 2: "c"}, ["event1", "event2"])}
        with patch.dict(sys.modules, {"evdev": types.SimpleNamespace(InputDevice=guard)}), \
             patch("kms_stress.hardware.receiver_nodes", return_value=nodes), \
             patch("kms_stress.hardware.selectors.DefaultSelector", return_value=FakeSelector()), \
             patch("kms_stress.hardware.os.open", side_effect=PermissionError("denied")):
            capture = Capture([Device("AABBCCDDEEFF", slot=4)], lambda *_: None)
            with self.assertRaises(PermissionError): capture.open()
            self.assertTrue(all(g.closed and not g.grabbed for g in guards))

    def test_missing_interface_blocks_capture(self):
        with patch.dict(sys.modules, {"evdev": types.SimpleNamespace(InputDevice=FakeGuard)}), \
             patch("kms_stress.hardware.receiver_nodes", return_value={}), \
             patch("kms_stress.hardware.selectors.DefaultSelector", return_value=FakeSelector()):
            with self.assertRaises(RuntimeError): Capture([Device("AABBCCDDEEFF", slot=4)], lambda *_: None).open()

    def test_feature_encoding_uses_configured_slot(self):
        calls = []
        with patch.dict(sys.modules, {"fcntl": types.SimpleNamespace(ioctl=lambda *args: calls.append(args))}):
            capture = Capture([], lambda *_: None); capture.handles[6, 2] = 306
            capture.feature(6, 1, 1234)
            fd, request, data = calls[0]
            self.assertEqual(fd, 306); self.assertEqual(len(data), 129); self.assertEqual(data[0], 126)
            self.assertEqual(struct.unpack_from("<4sII", data, 1), (b"KMT1", 1, 1234))
            self.assertEqual((request >> 16) & 0x3fff, 129)
            capture.handles.clear(); capture.close()

    def test_busy_hid_interface_yields_to_other_interfaces(self):
        capture = Capture([], lambda *_: None)
        first = types.SimpleNamespace(fd=1, data=(1, 0))
        second = types.SimpleNamespace(fd=2, data=(1, 1))
        reads = []
        def read(fd, size):
            reads.append(fd)
            if fd == 2: capture.stop.set()
            return b'\0'
        with patch.object(capture.selector, "select", return_value=[(first, 1), (second, 1)]), \
             patch("kms_stress.hardware.os.read", side_effect=read):
            capture._read()
        self.assertEqual(reads, [1] * 8 + [2])
        self.assertIsNone(capture.error)
        capture.close()

    def test_capture_serves_mouse_queue_before_delayed_keyboard_batch_overflows_it(self):
        keyboard = types.SimpleNamespace(fd=1, data=(1, 0))
        mouse = types.SimpleNamespace(fd=2, data=(1, 1))
        pending = list(range(32))
        keyboard_reads = mouse_reads = lost = 0
        observed = []
        capture = Capture([], lambda slot, interface, data, now: observed.append(data[0]) if interface == 1 else None)

        def read(fd, size):
            nonlocal keyboard_reads, mouse_reads, lost
            if fd == 1:
                keyboard_reads += 1
                if keyboard_reads == 20:
                    # A scheduling pause admits 48 reports at 333/s. The Linux
                    # mouse queue has 64 slots and must already have been drained.
                    arriving = list(range(32, 80))
                    room = 64 - len(pending)
                    pending.extend(arriving[:room])
                    lost += len(arriving) - room
                return b'\0'
            if not pending: raise BlockingIOError()
            value = pending.pop(0)
            mouse_reads += 1
            if keyboard_reads >= 20 and mouse_reads >= 17: capture.stop.set()
            return bytes([value])

        with patch.object(capture.selector, "select", return_value=[(keyboard, 1), (mouse, 1)]), \
             patch("kms_stress.hardware.os.read", side_effect=read):
            capture._read()
        self.assertIsNone(capture.error)
        self.assertEqual(lost, 0)
        self.assertEqual(observed + pending, list(range(80)))
        capture.close()

    def test_input_guards_drain_while_hid_verification_is_blocked(self):
        blocked, release, drained = threading.Event(), threading.Event(), threading.Event()
        def consume(*_):
            blocked.set(); release.wait(1)
        capture = Capture([], consume)
        hid = types.SimpleNamespace(fd=1, data=(1, 0))
        guard = types.SimpleNamespace(read=lambda: (drained.set() or []))
        event = types.SimpleNamespace(data=guard)
        with patch.object(capture.selector, "select", return_value=[(hid, 1)]), \
             patch.object(capture.guard_selector, "select", return_value=[(event, 1)]), \
             patch("kms_stress.hardware.os.read", return_value=b'\0'):
            capture.thread = threading.Thread(target=capture._read)
            capture.thread.start()
            self.assertTrue(blocked.wait(1))
            capture.guard_thread = threading.Thread(target=capture._read_guards)
            capture.guard_thread.start()
            try: self.assertTrue(drained.wait(1))
            finally:
                capture.stop.set(); release.set()
                capture.thread.join(1); capture.guard_thread.join(1)
        self.assertIsNone(capture.error)
        capture.close()

    def test_desktop_queue_loss_keeps_grab_and_continues_draining(self):
        log = Mock()
        capture = Capture([], lambda *_: None, log)
        guard = FakeGuard("event1"); guard.grab(); capture.guards.append(guard)
        def read():
            if read.drained:
                capture.stop.set()
                return [types.SimpleNamespace(type=0, code=0)]
            read.drained = True
            return [types.SimpleNamespace(type=0, code=3)]
        read.drained = False
        guard.read = read
        with patch.object(capture.guard_selector, "select", return_value=[(types.SimpleNamespace(data=guard), 1)]):
            capture._read_guards()
        self.assertIsNone(capture.error)
        self.assertTrue(capture.stop.is_set() and guard.grabbed)
        log.assert_called_once_with("capture-warning", {"warning": "desktop_event_queue_overrun", "device": "event1"})
        capture.close()
        self.assertFalse(guard.grabbed)

    def test_desktop_guard_read_failure_still_fails_capture(self):
        capture = Capture([], lambda *_: None)
        guard = types.SimpleNamespace(read=Mock(side_effect=OSError("device disappeared")))
        with patch.object(capture.guard_selector, "select", return_value=[(types.SimpleNamespace(data=guard), 1)]):
            capture._read_guards()
        self.assertIn("device disappeared", str(capture.error))
        capture.close()


class LifecycleTests(unittest.TestCase):
    def test_preflight_enters_mode_before_selecting_any_receiver(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(self.configuration(directory))
            runner.evidence = Mock()
            master, generator, capture = Mock(), Mock(), Mock()
            from kms_stress.hardware import FirmwareIdentity
            identities = [FirmwareIdentity("master", runner.config.master.identity, "12345678"),
                          FirmwareIdentity("generator", runner.config.generators[0].identity)]
            with patch("kms_stress.runner.SerialChannel", side_effect=[master, generator]), \
                 patch("kms_stress.runner.read_identity", side_effect=identities), \
                 patch("kms_stress.runner.peers", return_value=({6: runner.config.receivers[0].identity}, {}, 2, [])), \
                 patch("kms_stress.runner.Capture", return_value=capture), \
                 patch("kms_stress.runner.verify_inventory"), patch.object(runner, "wait"), \
                 patch.object(runner, "stop_generators"):
                runner.preflight()
            commands = [call.args[0] for call in master.request.call_args_list]
            self.assertLess(commands.index("test-mode on"), commands.index("select 6"))
            self.assertTrue(runner.test_mode_entered)

    def test_successful_cleanup_restores_everyday_startup_last(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(self.configuration(directory))
            events = []
            master, capture = Mock(), Mock()
            master.request.side_effect = lambda command, *args: events.append(command)
            capture.error = None
            capture.feature.side_effect = lambda *args: events.append("receiver off")
            def preflight():
                runner.master, runner.capture = master, capture
                runner.master_verified = runner.test_mode_entered = True
            with patch.object(runner, "preflight", side_effect=preflight), \
                 patch.object(runner, "schedule", return_value=[(6, 1, 1, "everyday", False)]), \
                 patch.object(runner, "segment", return_value={"status": "PASS", "slot": 6, "rate": 1, "profile": "everyday"}), \
                 patch.object(runner, "stop_generators", side_effect=lambda: events.append("neutral confirmed")), \
                 patch("kms_stress.runner.time.sleep"):
                result, _ = runner.run()
            self.assertEqual(result["status"], "PASS")
            self.assertLess(events.index("neutral confirmed"), events.index("test-mode off"))
            self.assertLess(events.index("receiver off"), events.index("test-mode off"))

    def test_failed_session_keeps_restart_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(self.configuration(directory))
            master = Mock()
            def preflight():
                runner.master = master
                runner.master_verified = runner.test_mode_entered = True
                raise RuntimeError("capture disconnected")
            with patch.object(runner, "preflight", side_effect=preflight), \
                 patch.object(runner, "collect_traces", return_value={}), patch.object(runner, "stop_generators"):
                result, _ = runner.run()
            self.assertFalse(any(call.args[0] == "test-mode off" for call in master.request.call_args_list))
            self.assertTrue(any("Test mode retained" in text for text in result["cleanup"]))

    def configuration(self, directory):
        return Config(master=Device("112233445566", uart="master"), generators=[Device("AABBCCDDEEFF", uart="generator")],
                      receivers=[Device("AABBCCDDEE01", slot=6)], output=directory)

    def test_preflight_failure_stops_sources_and_releases_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(self.configuration(directory))
            master, generator, capture = Mock(), Mock(), Mock()
            capture.traces.return_value = {}
            def preflight():
                runner.master, runner.generators, runner.capture = master, [generator], capture
                runner.master_verified = True; runner.verified_generators.add(generator)
                raise RuntimeError("simulated preflight failure")
            with patch.object(runner, "preflight", side_effect=preflight), \
                 patch("kms_stress.runner.master_traces", return_value={}), patch("kms_stress.runner.time.sleep"):
                result, path = runner.run()
            self.assertEqual(result["status"], "INCONCLUSIVE")
            self.assertTrue(any(call.args[0] == "bench stop" for call in generator.request.call_args_list))
            master.close.assert_called_once(); generator.close.assert_called_once(); capture.close.assert_called_once()
            self.assertTrue((path / "report.html").is_file())

    def test_unverified_identities_never_receive_control_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = Runner(self.configuration(directory))
            master, generator = Mock(), Mock()
            def preflight():
                runner.master, runner.generators = master, [generator]
                raise RuntimeError("identity mismatch")
            with patch.object(runner, "preflight", side_effect=preflight): result, _ = runner.run()
            master.request.assert_not_called(); generator.request.assert_not_called()
            self.assertEqual(result["status"], "INCONCLUSIVE")
            master.close.assert_called_once(); generator.close.assert_called_once()

    def test_cancel_does_not_become_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            cancel = threading.Event(); cancel.set()
            runner = Runner(self.configuration(directory), cancel=cancel)
            with patch.object(runner, "preflight", side_effect=runner.check):
                result, _ = runner.run()
            self.assertEqual(result["status"], "CANCELLED")

    def test_capture_loss_is_explicit(self):
        runner = Runner(Config()); runner.capture = types.SimpleNamespace(error=RuntimeError("USB disconnected"))
        with self.assertRaisesRegex(RuntimeError, "USB disconnected"): runner.check()

    def test_baseline_needs_no_master_or_receivers(self):
        config = Config(generators=[Device("AABBCCDDEEFF", uart="generator")], preset="baseline")
        config.validate()

    def test_live_capture_uses_configured_slots(self):
        runner = Runner(Config()); runner.segment_open, runner.active = True, 6
        runner.verifier = Mock()
        runner.consume(6, 0, bytes(34), 123)
        runner.verifier.feed.assert_called_once_with(0, bytes(34))
        runner.consume(3, 0, b"\x01" + bytes(33), 124)
        self.assertIn("slave 3", runner.leaks[0])

    def test_dashboard_initializes_without_starting_hardware(self):
        try:
            import tkinter as tk
            root = tk.Tk(); root.withdraw()
        except Exception as error: self.skipTest(f"Tk display unavailable: {error}")
        try:
            from kms_stress.gui import Dashboard
            with tempfile.TemporaryDirectory() as directory:
                with patch("kms_stress.discovery.SerialChannel") as serial:
                    dashboard = Dashboard(root, Path(directory) / "profile.json")
                    root.update_idletasks()
                    self.assertFalse(dashboard.busy()); serial.assert_not_called()
                    self.assertEqual(len(dashboard.rows), 1)
        finally: root.destroy()


if __name__ == "__main__": unittest.main()
