import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from kms_companion.geometry import Rect, crossing, normalize, parking_position
from kms_companion.protocol import Message, Op, encode_config, decode_config, default_config, words
from kms_companion.agent import Agent
from kms_companion.transport import DeviceError, RejectedCommand, Receiver


class ProtocolTests(unittest.TestCase):
    def test_heartbeat_reads_edge_receipt_and_clears_it_for_legacy_controller(self):
        receiver = Receiver.__new__(Receiver)
        receiver.diagnostics = False
        receiver.request = Mock(return_value=Message(Op.HEARTBEAT, data=words(1, 3, 4, 5, 91).ljust(48, b"\0")))
        self.assertEqual(receiver.heartbeat(True, True), (1, 3, 4, 5))
        self.assertEqual(receiver.edge_departure, 91)
        receiver.request.return_value = Message(Op.HEARTBEAT, data=words(1, 3, 4, 5).ljust(48, b"\0"))
        receiver.heartbeat(True, True)
        self.assertEqual(receiver.edge_departure, 0)

    def test_named_preferences_roundtrip(self):
        from kms_companion.preferences import encode, decode
        values = dict(trigger=1, normal_led=False, brightness=10, keyboard_leds=True,
            boot_slot=2, jiggler_seconds=30, double_ms=350, select_ms=3000,
            slot_colors=[1, 2, 3, 4, 5, 6, 7], jiggler_distance=1)
        self.assertEqual(decode(encode(values)), values)
    def test_reports_detect_every_corrupted_byte(self):
        wire = Message(Op.CROSS, 99, 123, words(1, 2, 3, 4)).encode()
        self.assertEqual(Message.decode(wire).id, 99)
        for i in range(64):
            bad = bytearray(wire); bad[i] ^= 1
            with self.assertRaises(ValueError):
                Message.decode(bad)

    def test_layout_roundtrip_and_validation(self):
        config = default_config()
        config["stations"][0].update(configured=True, x=-1366, y=-768)
        self.assertEqual(decode_config(encode_config(config)), config)
        for field, value in (("delay_ms", -1), ("corner", 100), ("enabled", 1)):
            bad = copy.deepcopy(config); bad[field] = value
            with self.assertRaises(ValueError):
                encode_config(bad)
        config["stations"][0]["x"] = 100001
        with self.assertRaises(ValueError):
            encode_config(config)

    def test_layout_requires_unique_slots(self):
        config = default_config(); config["stations"][1]["slot"] = 1
        with self.assertRaises(ValueError):
            encode_config(config)


class GeometryTests(unittest.TestCase):
    def test_all_parking_positions_on_single_screen_and_disconnected_edges(self):
        screens = [Rect(-100, -50, 101, 101)]
        expected = {"top-left": (-100, -50), "top-right": (0, -50), "top-centre": (-50, -50),
                    "bottom-left": (-100, 50), "bottom-right": (0, 50), "bottom-centre": (-50, 50),
                    "left-centre": (-100, 0), "right-centre": (0, 0)}
        for mode, point in expected.items():
            self.assertEqual(parking_position(screens, mode), point)
        screens = [Rect(-300, -200, 100, 100), Rect(200, -200, 100, 100), Rect(-300, 200, 100, 100)]
        for mode in expected:
            point = parking_position(screens, mode)
            self.assertTrue(any(r.contains(*point) for r in screens), (mode, point))
            self.assertEqual(parking_position(list(reversed(screens)), mode), point)
        self.assertEqual(parking_position(screens, "top-centre"), (-201, -200))
        self.assertEqual(parking_position(screens, "left-centre"), (-300, -101))

    def test_parking_uses_lowest_monitor_then_selected_side(self):
        layouts = (
            ([Rect(0, 0, 100, 100)], (99, 99), (0, 99)),
            ([Rect(-100, -50, 100, 100), Rect(0, -50, 200, 100)], (199, 49), (-100, 49)),
            ([Rect(0, 0, 100, 100), Rect(20, 100, 50, 100)], (69, 199), (20, 199)),
            # The bounding rectangle's bottom-right corner is empty space.
            ([Rect(-200, -100, 100, 300), Rect(0, 0, 300, 100)], (-101, 199), (-200, 199)),
            ([Rect(0, 0, 100, 100), Rect(0, 0, 100, 100)], (99, 99), (0, 99)),
        )
        for screens, right, left in layouts:
            for ordered in (screens, list(reversed(screens))):
                for mode, expected in (("bottom-right", right), ("bottom-left", left)):
                    with self.subTest(screens=ordered, mode=mode):
                        point = parking_position(ordered, mode)
                        self.assertEqual(point, expected)
                        self.assertTrue(any(r.contains(*point) for r in screens))
        self.assertIsNone(parking_position([], "bottom-right"))
        self.assertIsNone(parking_position(layouts[0][0], "disabled"))
        with self.assertRaises(ValueError):
            parking_position(layouts[0][0], "unknown")

    def test_four_directions(self):
        screens = [Rect(0, 0, 100, 100)]
        for point, motion, edge in (((0, 50), (-1, 0), 0), ((99, 50), (1, 0), 1),
                                    ((50, 0), (0, -1), 2), ((50, 99), (0, 1), 3)):
            self.assertEqual(crossing(screens, point, motion), edge)

    def test_internal_seam_and_exposed_offset_segment(self):
        screens = [Rect(0, 0, 100, 100), Rect(100, 50, 150, 100)]
        self.assertIsNone(crossing(screens, (99, 60), (1, 0)))
        self.assertEqual(crossing(screens, (99, 25), (1, 0)), 1)
        self.assertIsNone(crossing(screens, (0, 0), (-1, -1)))

    def test_negative_origins_and_arbitrary_monitor_count(self):
        screens, origin = normalize([Rect(-200, -100, 200, 100), Rect(0, 0, 300, 200)])
        self.assertEqual(origin, (-200, -100))
        self.assertEqual(screens[1], Rect(200, 100, 300, 200))
        for count in (1, 2, 4, 16):
            screens = [Rect(i * 100, 0, 100, 100) for i in range(count)]
            self.assertEqual(crossing(screens, (count * 100 - 1, 50), (1, 0)), 1)
            if count > 1:
                self.assertIsNone(crossing(screens, (99, 50), (1, 0)))


class FakeDesktop:
    name = "Fake X11"
    def __init__(self):
        self.rects = [Rect(0, 0, 100, 100)]
        self.point = (99, 50)
        self.is_ready = True
        self.moves = []
    def screens(self): return self.rects
    def pointer(self): return self.point
    def ready(self): return self.is_ready
    def place(self, x, y):
        self.moves.append((x, y)); self.point = x, y
        return self.is_ready


class FakeReceiver:
    def __init__(self):
        self.s = dict(slot=2, active=True, held=False, usb_ready=True, boot=1, master_boot=2,
            motion_sequence=1, dx=1, dy=0, event=0, event_x=0, event_y=0, event_geometry=0, generation=3)
        self.commands, self.heartbeats, self.geometries = [], [], []
        self.configuration = default_config()
        self.edge_departure = 0
    def status(self): return dict(self.s)
    def heartbeat(self, enabled, ready):
        self.heartbeats.append((enabled, ready)); return 1, 2, 3, 1
    def screens(self, rects, version): self.geometries.append((rects, version))
    def config(self): return self.configuration, 1
    def request(self, op, data=b"", **kw):
        self.commands.append((op, data)); return Message(op, id=len(self.commands))
    def close(self): pass


class AgentTests(unittest.TestCase):
    def test_close_failure_during_disconnect_still_reconnects(self):
        from unittest.mock import Mock
        previous = self.agent.receiver
        previous.close = Mock(side_effect=OSError('device already disconnected'))
        replacement = FakeReceiver()
        replacement.close = Mock()
        self.agent.receiver_factory = Mock(return_value=replacement)
        def finish():
            self.agent.stop_event.set()
        with patch.object(self.agent, 'step', side_effect=[DeviceError('connection lost'), finish]), \
             patch.object(self.agent.stop_event, 'wait'), \
             self.assertLogs('kms_companion.agent', level='ERROR'):
            self.agent.run()
        previous.close.assert_called_once_with()
        self.agent.receiver_factory.assert_called_once_with(None)
        replacement.close.assert_called_once_with()

    def test_unexpected_worker_failure_is_reported_and_stops_helper(self):
        with patch.object(self.agent, 'step', side_effect=TypeError('unexpected worker failure')), \
             self.assertLogs('kms_companion.agent', level='ERROR') as logs:
            self.agent.run()
        self.assertTrue(self.agent.stop_event.is_set())
        self.assertFalse(self.agent.status()['connected'])
        self.assertIn('unexpected worker failure', self.agent.status()['reason'])
        self.assertIn('Traceback', '\n'.join(logs.output))

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {"LOCALAPPDATA": self.directory.name, "XDG_CONFIG_HOME": self.directory.name})
        self.env.start()
        self.agent = Agent()
        self.agent.receiver, self.agent.desktop = FakeReceiver(), FakeDesktop()
        self.agent.desktop_error = "Not ready"
        self.agent.next_heartbeat = self.agent.next_screens = 0
        self.agent.edge_motion_at = 0
        self.clock = patch("kms_companion.agent.time.monotonic", return_value=10)
        self.time = self.clock.start()
        self.agent.step()
        self.agent.next_heartbeat = self.agent.next_screens = 100
        self.agent.cooldown = 0
    def tearDown(self):
        self.clock.stop(); self.env.stop(); self.directory.cleanup()
    def push(self, at):
        self.time.return_value = at
        self.agent.receiver.s["motion_sequence"] += 1
        self.agent.step()
    def test_only_fresh_routed_motion_triggers_crossing(self):
        self.push(11); self.push(11.2)
        self.assertEqual(self.agent.receiver.commands[-1][0], Op.CROSS)
        self.agent.receiver.commands.clear()
        self.time.return_value = 12; self.agent.step()
        self.assertEqual(self.agent.receiver.commands, [])
    def test_held_keys_disabled_inactive_and_locked_never_switch(self):
        for state in ({"held": True}, {"active": False}):
            self.agent.receiver.s.update(state); self.push(12); self.push(13)
            self.assertEqual(self.agent.receiver.commands, [])
            self.agent.receiver.s.update(held=False, active=True)
        self.agent.enabled = False; self.push(14); self.push(15)
        self.assertEqual(self.agent.receiver.commands, [])
    def test_geometry_change_is_reported_before_readiness(self):
        self.agent.desktop.rects.append(Rect(100, 0, 200, 100))
        self.agent.next_screens = 0
        self.agent.step()
        self.assertEqual(len(self.agent.receiver.geometries[-1][0]), 2)
        self.assertEqual(self.agent.receiver.heartbeats[-1], (True, False))
    def test_unavailable_desktop_does_not_replace_remembered_screens(self):
        before = list(self.agent.receiver.geometries)
        self.agent.desktop.rects = [Rect(0, 0, 800, 600)]
        self.agent.desktop.is_ready = False
        self.agent.next_heartbeat = self.agent.next_screens = 0
        self.agent.step()
        self.assertEqual(self.agent.receiver.geometries, before)
        self.assertEqual(self.agent.screens, [Rect(0, 0, 100, 100)])
        self.assertEqual(self.agent.receiver.heartbeats[-1], (True, False))
        self.agent.desktop.is_ready = True
        self.agent.step()
        self.assertEqual(self.agent.receiver.geometries[-1][0], [Rect(0, 0, 800, 600)])
    def test_failed_screen_save_is_retried_without_losing_old_geometry(self):
        self.agent.desktop.rects.append(Rect(100, 0, 200, 100))
        self.agent.next_screens = 0
        with patch.object(self.agent.receiver, "screens", side_effect=RejectedCommand("storage failure")):
            with self.assertRaises(RejectedCommand):
                self.agent.step()
        self.assertEqual(self.agent.screens, [Rect(0, 0, 100, 100)])
        self.agent.step()
        self.assertEqual(len(self.agent.receiver.geometries[-1][0]), 2)
    def test_arrival_requires_matching_geometry_and_is_idempotent(self):
        s = self.agent.receiver.s
        s.update(event=123, event_x=2, event_y=50, event_geometry=self.agent.geometry_version)
        self.agent.step(); self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [(2, 50)])
        self.assertEqual(len([q for q in self.agent.receiver.commands if q[0] == Op.READY]), 1)
    def test_stale_arrival_does_not_warp_pointer(self):
        self.agent.receiver.s.update(event=123, event_geometry=0)
        self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [])
    def test_arrival_confirmation_precedes_background_radio_work(self):
        calls = []
        self.agent.receiver.s.update(event=123, event_x=2, event_y=50,
            event_geometry=self.agent.geometry_version, clipboard=True)
        self.agent.next_heartbeat = self.agent.next_screens = 0
        self.agent.receiver.request = Mock(side_effect=lambda op, *args, **kw: calls.append(op))
        self.agent.receiver.heartbeat = Mock(side_effect=AssertionError("Heartbeat delayed arrival"))
        self.agent.clipboard.pump = Mock(side_effect=AssertionError("Clipboard discovery delayed arrival"))
        self.agent.step()
        self.assertEqual(calls, [Op.READY])
        self.assertEqual(self.agent.desktop.moves, [(2, 50)])
        self.agent.receiver.heartbeat.assert_not_called()
        self.agent.clipboard.pump.assert_not_called()
        # Normal background work resumes on the following poll.
        self.agent.next_heartbeat = self.agent.next_screens = 100
        self.agent.clipboard.pump.side_effect = None
        self.agent.step()
        self.agent.clipboard.pump.assert_called_once()
    def test_local_disable_is_persisted_and_announced(self):
        self.agent.execute("auto", {"enabled": False})
        self.assertFalse(Agent().enabled)
        self.assertFalse(self.agent.receiver.heartbeats[-1][0])
    def test_dongle_identity_is_read_from_live_status(self):
        self.agent.receiver.s["slot"] = 3
        self.agent.next_heartbeat = 0
        self.agent.step()
        self.assertEqual(self.agent.status()["slot"], 3)

    def test_master_restart_reopens_receiver_and_republishes_unchanged_screens(self):
        previous = self.agent.receiver
        previous.s["diagnostics"] = True
        previous.heartbeat = Mock(side_effect=DeviceError("Master did not acknowledge command"))
        previous.close = Mock()
        replacement = FakeReceiver()
        replacement.s.update(diagnostics=True, master_boot=3)
        self.agent.receiver_factory = Mock(return_value=replacement)
        self.agent.desktop_factory = lambda: self.agent.desktop
        self.agent.next_heartbeat = 0
        self.agent.next_recovery = 100
        heartbeat = replacement.heartbeat

        def recovered(enabled, ready):
            if ready and replacement.geometries:
                self.agent.stop_event.set()
            return heartbeat(enabled, ready)

        replacement.heartbeat = recovered
        waits = 0

        def bounded_wait(_):
            nonlocal waits
            waits += 1
            if waits > 10:
                self.agent.stop_event.set()

        with patch.object(self.agent.stop_event, "wait", side_effect=bounded_wait):
            self.agent.run()

        self.agent.receiver_factory.assert_called_once_with(None)
        previous.close.assert_called_once_with()
        self.assertEqual(replacement.geometries, [([Rect(0, 0, 100, 100)], self.agent.geometry_version)])
        self.assertTrue(self.agent.status()["master_connected"])
        self.assertTrue(self.agent.status()["ready"])
        self.assertEqual(self.agent.status()["master_boot"], 3)

    def test_stale_master_heartbeat_uses_connection_recovery(self):
        self.agent.receiver.s["diagnostics"] = True
        self.agent.receiver.heartbeat = Mock(side_effect=RejectedCommand("stale session"))
        self.agent.next_heartbeat = 0
        with self.assertRaises(DeviceError) as raised:
            self.agent.step()
        self.assertNotIsInstance(raised.exception, RejectedCommand)

    def begin_edge_handoff(self):
        self.agent.step()
        self.agent.desktop.point = (99, 50)
        self.agent.cooldown, self.agent.edge = 0, None
        self.push(11); self.push(11.2)
        self.assertEqual(self.agent.receiver.commands[-1][0], Op.CROSS)
        self.agent.receiver.commands.clear()

    def depart(self, selected=3, edge=True):
        if edge:
            self.begin_edge_handoff()
            self.agent.receiver.edge_departure = self.agent.parking_request[0]
        else:
            self.agent.receiver.edge_departure = 0
        self.agent.receiver.heartbeat = Mock(return_value=(1, selected, 4, 1))
        self.agent.receiver.s["active"] = False
        self.agent.step()

    def test_departure_parks_once_and_leaves_local_mouse_free(self):
        self.depart()
        self.assertEqual(self.agent.desktop.moves, [(99, 99)])
        self.agent.desktop.point = (40, 50)  # A directly connected mouse moves.
        self.push(11)
        self.assertEqual(self.agent.desktop.point, (40, 50))
        self.assertEqual(self.agent.desktop.moves, [(99, 99)])
        self.agent.receiver.heartbeat.assert_called_once()
        self.assertEqual(self.agent.receiver.commands, [])

    def test_parking_uses_current_screens_after_an_edge_handoff(self):
        self.begin_edge_handoff()
        self.agent.receiver.edge_departure = self.agent.parking_request[0]
        self.agent.pointer_parking = "bottom-left"
        self.agent.desktop.rects = [Rect(-200, -100, 100, 300), Rect(0, 0, 300, 100)]
        self.agent.receiver.heartbeat = Mock(return_value=(1, 3, 4, 1))
        self.agent.receiver.s["active"] = False
        self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [(-200, 199)])

    def test_new_parking_positions_move_pointer_after_confirmed_edge_switch(self):
        expected = {"top-right": (99, 0), "top-left": (0, 0), "top-centre": (49, 0),
                    "bottom-centre": (49, 99), "left-centre": (0, 49), "right-centre": (99, 49)}
        for mode, point in expected.items():
            with self.subTest(mode=mode):
                self.agent.receiver.s["active"] = True
                self.agent.pointer_parking = mode
                self.agent.desktop.moves.clear()
                self.begin_edge_handoff()
                self.agent.receiver.edge_departure = self.agent.parking_request[0]
                self.agent.receiver.heartbeat = Mock(return_value=(1, 3, 4, 1))
                self.agent.receiver.s["active"] = False
                self.agent.step()
                self.assertEqual(self.agent.desktop.moves, [point])

    def test_keyboard_and_manual_switches_do_not_park(self):
        for automatic in (True, False):
            with self.subTest(automatic=automatic):
                self.agent.enabled = automatic
                self.agent.receiver.s["active"] = True
                self.agent.step()
                self.depart(edge=False)
                self.assertEqual(self.agent.desktop.moves, [])
                self.agent.receiver.heartbeat.assert_not_called()

    def test_manual_override_or_older_controller_does_not_confirm_pending_edge(self):
        self.begin_edge_handoff()
        self.depart(edge=False)  # Accepted CROSS was cancelled by a keyboard switch.
        self.assertEqual(self.agent.desktop.moves, [])
        self.agent.receiver.heartbeat.assert_called_once()
        # A later manual departure must not reuse the previous accepted CROSS.
        self.agent.receiver.s["active"] = True
        self.agent.step()
        self.depart(edge=False)
        self.agent.receiver.heartbeat.assert_not_called()
        self.assertEqual(self.agent.desktop.moves, [])

    def test_cli_selection_cancels_pending_edge_parking(self):
        self.begin_edge_handoff()
        self.agent.receiver.edge_departure = self.agent.parking_request[0]
        self.agent.execute("select", dict(slot=3))
        self.agent.receiver.s["active"] = False
        self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [])

    def test_disabled_locked_or_unavailable_desktop_does_not_park(self):
        for condition in ("disabled", "locked", "unavailable", "usb", "master"):
            with self.subTest(condition=condition):
                self.agent.receiver.s.update(active=True, usb_ready=True, master_boot=2)
                self.agent.desktop = FakeDesktop()
                self.agent.pointer_parking = "bottom-right"
                self.begin_edge_handoff()
                self.agent.receiver.edge_departure = self.agent.parking_request[0]
                self.agent.receiver.heartbeat = Mock(return_value=(1, 3, 4, 1))
                self.agent.pointer_parking = "disabled" if condition == "disabled" else "bottom-right"
                desktop = self.agent.desktop = FakeDesktop()
                desktop.is_ready = condition != "locked"
                if condition == "unavailable":
                    self.agent.desktop = None
                self.agent.receiver.s.update(active=True, usb_ready=condition != "usb", master_boot=0 if condition == "master" else 2)
                self.agent.step()
                self.agent.receiver.s["active"] = False
                self.agent.step()
                self.assertEqual(desktop.moves, [])

    def test_startup_reconnection_or_changed_identity_does_not_park(self):
        for change in ({"boot": 20}, {"master_boot": 30}, {"slot": 3}, {}):
            with self.subTest(change=change):
                self.agent.receiver.s.update(active=True)
                self.agent.step()
                self.begin_edge_handoff()
                self.agent.receiver.edge_departure = self.agent.parking_request[0]
                self.agent.receiver.heartbeat = Mock(return_value=(1, 3, 4, 1))
                self.agent.receiver.s.update(change)
                if not change:
                    # A fresh connection discards the old observation before polling.
                    self.agent.last_active, self.agent.receiver_identity = False, None
                self.agent.receiver.s["active"] = False
                self.agent.step()
                self.assertEqual(self.agent.desktop.moves, [])

    def test_lease_expiry_or_no_selected_route_does_not_park(self):
        for selected in (0, 2):
            with self.subTest(selected=selected):
                self.agent.receiver.s["active"] = True
                self.agent.step()
                self.depart(selected)
                self.assertEqual(self.agent.desktop.moves, [])

    def test_failed_handoff_keeps_source_pointer_in_place(self):
        self.push(11)
        with patch.object(self.agent.receiver, "request", side_effect=RejectedCommand("No destination")):
            with self.assertRaises(RejectedCommand):
                self.push(11.2)
        self.push(12)
        self.assertEqual(self.agent.desktop.moves, [])

    def test_parking_confirmation_failure_is_not_retried_or_disconnect_helper(self):
        for error in (DeviceError("Master unavailable"), OSError("Placement failed")):
            with self.subTest(error=error):
                self.agent.receiver.s["active"] = True
                self.agent.step()
                self.begin_edge_handoff()
                self.agent.receiver.edge_departure = self.agent.parking_request[0]
                self.agent.receiver.s["active"] = False
                with patch.object(self.agent.receiver, "heartbeat", side_effect=error) as heartbeat:
                    self.agent.step()
                    self.agent.step()
                    heartbeat.assert_called_once()
                self.assertIsNotNone(self.agent.receiver)
                self.assertEqual(self.agent.desktop.moves, [])
                self.assertEqual(self.agent.status()["pointer_parking_error"], str(error))

    def test_rejected_placement_does_not_retry_or_disconnect_helper(self):
        with patch.object(self.agent.desktop, "place", return_value=False) as place:
            self.depart()
            self.agent.step()
            place.assert_called_once_with(99, 99)
        self.assertIsNotNone(self.agent.receiver)
        self.assertEqual(self.agent.status()["pointer_parking_error"], "Desktop rejected pointer parking")

    def test_resume_does_not_park_for_a_departure_while_asleep(self):
        from kms_companion.power import PowerState
        self.agent.power_state = PowerState.SLEEP_OR_HIBERNATE
        self.agent.step()
        self.agent.receiver.s["active"] = False
        self.agent.power_state = PowerState.AWAKE
        self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [])

    def test_local_movement_or_return_during_confirmation_cancels_parking(self):
        for change in ("mouse", "active", "boot", "master_boot", "generation", "event", "usb_ready"):
            with self.subTest(change=change):
                self.agent.receiver.s.update(active=True, usb_ready=True)
                self.agent.step()
                self.begin_edge_handoff()
                self.agent.receiver.edge_departure = self.agent.parking_request[0]
                self.agent.receiver.s["active"] = False
                def confirm(*args):
                    if change == "mouse":
                        self.agent.desktop.point = (20, 30)
                    elif change in ("active", "usb_ready"):
                        self.agent.receiver.s[change] = change == "active"
                    else:
                        self.agent.receiver.s[change] += 1
                    return 1, 3, 4, 1
                with patch.object(self.agent.receiver, "heartbeat", side_effect=confirm):
                    self.agent.step()
                self.assertEqual(self.agent.desktop.moves, [])
                # Avoid treating a test event as a new arrival on the next iteration.
                self.agent.last_event = self.agent.receiver.s["event"]

    def test_arrival_after_parking_uses_existing_entry_placement(self):
        self.depart()
        self.agent.receiver.s.update(event=123, event_x=2, event_y=50,
                                     event_geometry=self.agent.geometry_version)
        self.agent.step()
        self.agent.receiver.s["active"] = True
        self.agent.step()
        self.assertEqual(self.agent.desktop.moves, [(99, 99), (2, 50)])
        self.assertEqual(self.agent.receiver.commands[-1], (Op.READY, words(123, 0)))

    def test_parking_preferences_default_validate_and_preserve_other_settings(self):
        self.assertEqual(self.agent.execute("status", {})["pointer_parking"], "bottom-right")
        for mode in ("bottom-left", "disabled", "bottom-right"):
            self.agent.execute("pointer-parking", dict(mode=mode))
            self.agent.execute("auto", dict(enabled=False))
            self.assertEqual(Agent().pointer_parking, mode)
            self.assertFalse(Agent().enabled)
        with self.assertRaises(ValueError):
            self.agent.execute("pointer-parking", dict(mode="invalid"))
        with patch.object(self.agent, "save_local_preferences", side_effect=OSError("Disk full")):
            with self.assertRaises(OSError):
                self.agent.execute("pointer-parking", dict(mode="disabled"))
        self.assertEqual(self.agent.pointer_parking, "bottom-right")
        for document in ({"enabled": False}, {"pointer_parking": "invalid"}):
            self.agent.local_file.write_text(json.dumps(document))
            self.assertEqual(Agent().pointer_parking, "bottom-right")

    def test_parking_cli_uses_local_helper(self):
        from kms_companion.cli import execute, parser
        with patch("kms_companion.cli.ipc.call", return_value={}) as call:
            execute(parser().parse_args(["pointer-parking", "bottom-left"]))
        call.assert_called_once_with("pointer-parking", mode="bottom-left")


if __name__ == "__main__":
    unittest.main()
