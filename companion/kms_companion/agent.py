"""Single-owner worker. IPC, CLI and GUI submit commands to this worker."""
from concurrent.futures import Future
import json
import logging
import os
from pathlib import Path
import queue
import secrets
import struct
import subprocess
import threading
import time
from .desktop import create_desktop, UnsupportedDesktop
from .geometry import normalize, crossing, parking_position, POINTER_PARKING_MODES
from .protocol import Op, words
from .transport import Receiver, DeviceError, RejectedCommand
from .power import PowerMonitor, PowerState
from .diagnostics import JournalReader, RecoveryNotices


def config_directory():
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".config")) if os.name == "nt" else Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "KMSwitcher"


class Agent:
    def __init__(self, serial=None, desktop_factory=create_desktop, receiver_factory=Receiver):
        self.directory = config_directory()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.local_file = self.directory / "preferences.json"
        self.enabled = True
        self.recovery_notifications = True
        self.pointer_parking = "bottom-right"
        try:
            local = json.loads(self.local_file.read_text())
            self.enabled = bool(local.get('enabled', True))
            self.recovery_notifications = bool(local.get('recovery_notifications', True))
            if local.get("pointer_parking") in POINTER_PARKING_MODES:
                self.pointer_parking = local["pointer_parking"]
        except (OSError, ValueError, KeyError):
            pass
        self.serial, self.desktop_factory, self.receiver_factory = serial, desktop_factory, receiver_factory
        self.jobs = queue.Queue()
        self.stop_event = threading.Event()
        self.snapshot = {"connected": False, "enabled": self.enabled, "reason": "Starting"}
        self.snapshot_lock = threading.Lock()
        self.receiver = None
        self.desktop = None
        self.config = None
        self.revision = 0
        self.screens = []
        self.origin = (0, 0)
        self.geometry_version = secrets.randbits(30) or 1
        self.cooldown = 0
        self.last_event = 0
        self.last_motion = None
        self.last_active = False
        self.receiver_identity = None
        self.parking_request = None
        self.edge = None
        self.edge_at = 0
        self.power_state = PowerState.UNKNOWN
        self.power_lock = threading.Lock()
        self.power_monitor = PowerMonitor(self.notify_power)
        self.next_power = 0
        self.journal = JournalReader()
        self.recovery_notices = RecoveryNotices()
        self.next_recovery = 0
        self.recovery_slot = 1
        self.recovery_unsupported = None
        from .clipboard_service import TransferService
        self.clipboard = TransferService(self.directory)

    def start(self):
        self.clipboard.start()
        self.thread = threading.Thread(target=self.run, daemon=True, name="kms-hid-owner")
        self.thread.start()
        self.power_monitor.start()

    def notify_power(self, state):
        with self.power_lock:
            self.power_state = state
        future = Future()
        self.jobs.put(("power-notify", {}, future))
        try:
            future.result(timeout=.8)
        except TimeoutError:
            pass  # The state remains pending if the HID owner is busy or disconnected.
        except (OSError, DeviceError, ValueError) as error:
            self.update(power_report_error=str(error))

    def report_power(self, status):
        if not status.get("power_supported"):
            return
        with self.power_lock:
            state = self.power_state
        self.receiver.power(state)
        self.next_power = time.monotonic() + .7

    def call(self, command, **args):
        if command.startswith("clipboard-"):
            # IPC workers handle disk/staging operations independently of the HID owner.
            return self.execute(command, args)
        future = Future()
        self.jobs.put((command, args, future))
        return future.result(timeout=20)

    def update(self, **values):
        with self.snapshot_lock:
            self.snapshot.update(values)

    def status(self):
        with self.snapshot_lock:
            return dict(self.snapshot)

    def close(self):
        self.power_monitor.close()
        self.stop_event.set()
        if hasattr(self, "thread"):
            self.thread.join(timeout=4)
        self.clipboard.close()

    def execute(self, command, args):
        if command == "power-notify":
            self.next_power = self.next_heartbeat = 0
            if self.receiver:
                self.report_power(self.receiver.status())
            return {}
        if command == "stop":
            self.stop_event.set()
            return {"stopping": True}
        if command == "status":
            return dict(self.status(), recovery_notifications=self.recovery_notifications,
                        pointer_parking=self.pointer_parking,
                        recovery_session=self.recovery_notices.session,
                        recovery_events=list(self.recovery_notices.events))
        if command == 'diagnostics-status':
            return dict(self.journal.state, entries=list(self.journal.state['entries']), recovery_events=list(self.recovery_notices.events))
        if command == 'diagnostics-refresh':
            if not self.receiver or not getattr(self.receiver, 'diagnostics', False):
                raise DeviceError('Local logs require a connected S2 with updated firmware')
            self.journal.start(self.receiver)
            return dict(self.journal.state)
        if command == 'recovery-notifications':
            previous = self.recovery_notifications
            self.recovery_notifications = bool(args['enabled'])
            try: self.save_local_preferences()
            except OSError:
                self.recovery_notifications = previous
                raise
            return dict(enabled=self.recovery_notifications)
        if command == "pointer-parking":
            mode = args["mode"]
            if mode not in POINTER_PARKING_MODES:
                raise ValueError("Unknown pointer parking mode")
            previous = self.pointer_parking
            self.pointer_parking = mode
            try:
                self.save_local_preferences()
            except OSError:
                self.pointer_parking = previous
                raise
            return dict(pointer_parking=self.pointer_parking)
        if command == "clipboard-status":
            return self.clipboard.status(ui=bool(args.get("ui")))
        if command == "clipboard-settings":
            return self.clipboard.configure(args["settings"])
        if command == "clipboard-send":
            return self.clipboard.send(args["slot"], args["capture"])
        if command == "clipboard-auto-send":
            return self.clipboard.send_automatic(args["capture"], args["source_session"])
        if command == "clipboard-cancel":
            return self.clipboard.cancel(args["id"])
        if command == "clipboard-dismiss":
            return self.clipboard.dismiss(args["id"], release=bool(args.get("release")))
        if command == "clipboard-clear":
            return self.clipboard.clear(args.get("kind"))
        if command == "clipboard-inspect":
            return self.clipboard.inspect(args["id"])
        if command == "clipboard-content":
            return self.clipboard.content(args["id"])
        if command == "clipboard-applied":
            return self.clipboard.applied(args["id"])
        if command == "clipboard-save":
            return self.clipboard.save(args["id"], args["parent"], automatic=bool(args.get("automatic")))
        if command == "auto":
            self.enabled = bool(args["enabled"])
            self.save_local_preferences()
            self.update(enabled=self.enabled)
            if self.receiver:
                self.receiver.heartbeat(self.enabled, bool(self.desktop and self.desktop.ready()))
            return {"enabled": self.enabled}
        if not self.receiver:
            raise DeviceError("No receiver connected")
        if command.startswith('device-'):
            from .device_control import execute
            return execute(self.receiver, command, args)
        if command == 'preferences-default':
            from .preferences import decode
            from .device_control import request
            r = request(self.receiver, Op.PREFERENCES_DEFAULT, words(args['revision']))
            return dict(revision=struct.unpack_from('<I', r.data)[0], values=decode(r.data[4:36]))
        if command == "config-get":
            self.config, self.revision = self.receiver.config()
            self.next_heartbeat = 0
            return {"config": self.config, "revision": self.revision}
        if command == "config-set":
            options = {"refresh_joins": True} if args.get("refresh_joins") else {}
            self.revision = self.receiver.save_config(args["config"], args["revision"], **options)
            self.config, self.revision = self.receiver.config()
            self.next_heartbeat = 0
            return {"config": self.config, "revision": self.revision}
        if command == "peers":
            if args.get("include_joins"):
                return self.receiver.peers(include_joins=True)
            return self.receiver.peers()
        if command == "select":
            self.parking_request = None
            self.receiver.request(Op.SELECT, words(args["slot"]))
            return {"selected": args["slot"]}
        if command == "preferences-get":
            from .preferences import decode
            r = self.receiver.request(Op.PREFERENCES_GET)
            return {"revision": struct.unpack_from("<I", r.data)[0], "values": decode(r.data[4:36])}
        if command in ("channel-get", "channel-set", "channel-resume", "channel-recovery-get", "channel-recovery-set"):
            from .radio_channel import request_channel
            return request_channel(self.receiver, command, args)
        if command == "preferences-set":
            from .preferences import encode
            data = encode(args["values"])
            self.receiver.request(Op.PREFERENCES_SET, words(args["revision"]) + data)
            return self.execute("preferences-get", {})
        raise ValueError("Unknown command")

    def save_local_preferences(self):
        temp = self.local_file.with_suffix('.tmp')
        temp.write_text(json.dumps(dict(enabled=self.enabled, recovery_notifications=self.recovery_notifications,
                                       pointer_parking=self.pointer_parking)), encoding='utf-8')
        os.replace(temp, self.local_file)

    @staticmethod
    def connection_identity(status):
        return status["boot"], status["master_boot"], status["slot"]

    def park_pointer(self, status, request_id):
        """Park once, only when the controller confirms this CROSS completed."""
        if self.pointer_parking == "disabled" or not self.desktop or not status["usb_ready"] or not status["master_boot"]:
            return
        try:
            if not self.desktop.ready():
                return
            point = self.desktop.pointer()
            _, selected, _, _ = self.receiver.heartbeat(self.enabled, bool(self.screens))
            current = self.receiver.status()
            if (getattr(self.receiver, "edge_departure", 0) != request_id or
                    not 1 <= selected <= 7 or selected == status["slot"] or current["active"] or
                    not current["usb_ready"] or self.connection_identity(current) != self.connection_identity(status) or
                    current["generation"] != status["generation"] or current["event"] != status["event"]):
                return
            target = parking_position(self.desktop.screens(), self.pointer_parking)
            # A directly connected mouse may have moved during the controller round trip.
            if target is not None and self.desktop.pointer() == point:
                if not self.desktop.place(*target):
                    raise OSError("Desktop rejected pointer parking")
                self.update(pointer_parking_error=None)
        except (OSError, DeviceError, ValueError, UnsupportedDesktop, subprocess.SubprocessError) as error:
            # Cosmetic placement must never disconnect the helper or retry during local use.
            self.update(pointer_parking_error=str(error))

    def service_jobs(self):
        for _ in range(4):
            try:
                command, args, future = self.jobs.get_nowait()
            except queue.Empty:
                return
            try:
                future.set_result(self.execute(command, args))
            except Exception as error:
                future.set_exception(error)

    def step(self):
        now = time.monotonic()
        s = self.receiver.status()
        if now >= self.next_power:
            self.report_power(s)
        with self.power_lock:
            awake = self.power_state in (PowerState.UNKNOWN, PowerState.AWAKE)
        if not awake:
            self.edge = None
            self.last_active, self.receiver_identity = False, None
            self.parking_request = None
            self.update(connected=True, **s, power_notifications=self.power_monitor.reason)
            return
        self.journal.tick(self.receiver)
        if s.get('diagnostics') and self.snapshot.get('master_connected') is False and now < self.next_heartbeat:
            return
        identity = self.connection_identity(s)
        departed = self.last_active and not s["active"] and identity == self.receiver_identity
        parking_request = self.parking_request
        if identity != self.receiver_identity or self.last_active != s["active"]:
            self.parking_request = None
        self.receiver_identity = identity
        if self.last_active != s["active"]:
            self.cooldown, self.edge = now + .35, None
            self.last_active = s["active"]
        if self.desktop and s["event"] and s["event"] != self.last_event:
            self.parking_request = None
            self.last_event = s["event"]
            ready = False
            if self.enabled and s["event_geometry"] == self.geometry_version and self.desktop.ready():
                ready = self.desktop.place(s["event_x"] + self.origin[0], s["event_y"] + self.origin[1])
            self.receiver.request(Op.READY, words(s["event"], 0 if ready else 1), timeout=.25)
            self.cooldown, self.edge = now + .5, None
            # Background peer discovery can block on a radio round trip. Finish
            # the arrival before starting that work or another heartbeat.
            return
        if departed and parking_request and parking_request[1:] == (identity, s["generation"]):
            self.park_pointer(s, parking_request[0])
        if s.get("clipboard"):
            self.clipboard.pump(self.receiver, s, bool(self.desktop and self.desktop.ready()))
        if now >= self.next_heartbeat:
            ready = bool(self.desktop and self.desktop.ready() and self.screens)
            try:
                revision, selected, generation, topology_revision = self.receiver.heartbeat(self.enabled, ready)
            except DeviceError as error:
                if not s.get('diagnostics'): raise
                if s.get('master_boot'):
                    # Reopen with a fresh helper session so the S2 forgets the
                    # old master epoch and the worker republishes its screens.
                    # Convert stale-session rejections into connection recovery too.
                    raise DeviceError('Master connection lost; reconnecting') from error
                self.update(connected=True, master_connected=False, **s, enabled=self.enabled, ready=False,
                            reason='Master unavailable; local logs are available')
                self.next_heartbeat = now + 2
                return
            if self.revision != revision:
                self.config, self.revision = self.receiver.config()
            self.update(connected=True, master_connected=True, **s, enabled=self.enabled, ready=ready, selected=selected,
                        serial=getattr(self.receiver, "info", {}).get("serial_number"),
                        topology_revision=topology_revision,
                        backend=self.desktop.name if self.desktop else "Unavailable", revision=revision,
                        reason="Ready" if ready else self.desktop_error, power_notifications=self.power_monitor.reason)
            self.next_heartbeat = now + .7
        if s.get('diagnostics') and s.get('master_boot') and now >= self.next_recovery and not self.journal.phase and self.recovery_unsupported != s['master_boot']:
            try:
                data = self.receiver.request(Op.RECOVERY_STATUS, words(self.recovery_slot), timeout=.12).data
                if struct.unpack_from('<I', data, 16)[0] != self.recovery_slot:
                    raise ValueError('Recovery status belongs to another dongle')
                self.recovery_notices.observe(data, now, self.recovery_notifications)
            except RejectedCommand:
                self.recovery_unsupported = s['master_boot']
            except (OSError, DeviceError, ValueError):
                pass  # A status probe never disconnects the USB owner or creates a popup.
            self.recovery_slot = self.recovery_slot % 7 + 1
            self.next_recovery = now + 1
        if self.desktop and now >= self.next_screens and self.desktop.ready():
            rects, origin = normalize(self.desktop.screens())
            if rects != self.screens or origin != self.origin:
                self.geometry_version = (self.geometry_version + 1) & 0xffffffff or 1
                self.receiver.heartbeat(self.enabled, False)
                self.receiver.screens(rects, self.geometry_version)
                self.screens, self.origin = rects, origin
                self.update(screens=[r.as_dict() for r in rects])
                self.cooldown = now + .5
                self.next_heartbeat = 0
            self.next_screens = now + 1
        if not self.desktop or not self.enabled or not self.config or not self.config["enabled"] or not s["active"] or s["held"] or not self.desktop.ready() or now < self.cooldown:
            self.edge = None
            self.last_motion = s["motion_sequence"]
            return
        fresh = self.last_motion != s["motion_sequence"]
        self.last_motion = s["motion_sequence"]
        if not fresh:
            if now - self.edge_motion_at > .12:
                self.edge = None
            return
        point = self.desktop.pointer()
        local = point[0] - self.origin[0], point[1] - self.origin[1]
        direction = crossing(self.screens, local, (s["dx"], s["dy"]), self.config["corner"])
        self.edge_motion_at = now
        if direction is None:
            self.edge = None
        elif self.edge != direction:
            self.edge, self.edge_at = direction, now
        if direction is not None and now - self.edge_at >= self.config["delay_ms"] / 1000:
            self.edge = None
            self.cooldown = now + .5
            self.parking_request = None
            reply = self.receiver.request(Op.CROSS, words(s["generation"], direction, *local, self.geometry_version), timeout=.4)
            self.parking_request = (reply.id, identity, s["generation"])

    def _disconnect_receiver(self):
        receiver, self.receiver = self.receiver, None
        if receiver:
            try:
                receiver.close()
            except Exception:
                logging.getLogger(__name__).exception('Failed to close disconnected receiver')

    def run(self):
        try:
            self._run_session()
        except Exception as error:
            logging.getLogger(__name__).exception('HID owner stopped unexpectedly')
            self.update(reason=f'Helper error: {error}', connected=False, ready=False)
            self.stop_event.set()
        finally:
            self._disconnect_receiver()

    def _run_session(self):
        self.desktop_error = "Desktop unavailable"
        try:
            self.desktop = self.desktop_factory()
        except (OSError, UnsupportedDesktop) as error:
            self.desktop_error = str(error)
        self.edge_motion_at = 0
        while not self.stop_event.is_set():
            self.service_jobs()
            try:
                if self.receiver is None:
                    self.receiver = self.receiver_factory(self.serial)
                    self.screens, self.last_event, self.last_motion = [], 0, None
                    self.last_active, self.receiver_identity = False, None
                    self.parking_request = None
                    self.next_heartbeat = self.next_screens = 0
                    self.next_power = 0
                    self.cooldown = time.monotonic() + .5
                self.step()
            except RejectedCommand as error:
                self.update(last_handoff=str(error))
            except (OSError, DeviceError, ValueError, UnsupportedDesktop, subprocess.SubprocessError) as error:
                self.update(reason=str(error), connected=False, ready=False)
                self._disconnect_receiver()
                self.journal.cancel('Dongle disconnected; refresh after it reconnects')
                self.clipboard.disconnected()
                self.stop_event.wait(.5)
            self.stop_event.wait(.005)
        if self.receiver:
            try:
                # Stopping the helper is not a PC shutdown. Retain actual OS transition intent.
                if self.power_state in (PowerState.UNKNOWN, PowerState.AWAKE):
                    self.power_state = PowerState.UNKNOWN
                    self.report_power(self.receiver.status())
                self.receiver.heartbeat(False, False)
            except (OSError, DeviceError, ValueError):
                pass
