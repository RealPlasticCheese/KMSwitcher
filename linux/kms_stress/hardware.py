"""Linux device adapters. Imports needing hardware are deliberately lazy."""
from dataclasses import dataclass, field
import os
import queue
import re
import selectors
import struct
import threading
import time

from .model import Device, trace_page


class SerialChannel:
    def __init__(self, path, log=lambda *args: None):
        import serial
        self.path, self.log = path, log
        self.lines = queue.Queue(4096)
        self.error = None
        self.restarted = False
        self.last_boot_id = None
        self.closed = threading.Event()
        self.command_lock = threading.Lock()
        self.port = serial.Serial()
        self.port.port, self.port.baudrate = path, 115200
        self.port.timeout, self.port.write_timeout = 0.1, 1
        self.port.dtr = self.port.rts = False
        self.port.exclusive = True
        self.port.open()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        pending = bytearray()
        try:
            while not self.closed.is_set():
                pending.extend(self.port.read(max(1, self.port.in_waiting)))
                if len(pending) > 65536: raise RuntimeError(f"UART framing overrun: {self.path}")
                while b"\n" in pending:
                    raw, _, rest = pending.partition(b"\n")
                    pending = bytearray(rest)
                    line = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode("utf-8", "replace")).strip()
                    if not line: continue
                    if line.startswith(("KMSBENCH BOOT ", "KMSISO BOOT ")):
                        self.restarted = True; self.last_boot_id = line.split()[-1].lower()
                    self.log("uart", {"port": self.path, "line": line})
                    try: self.lines.put_nowait(line)
                    except queue.Full: raise RuntimeError(f"UART collector overrun: {self.path}")
        except Exception as error:
            if not self.closed.is_set(): self.error = error

    def request(self, command, prefix=None, timeout=3, service=None):
        with self.command_lock:
            if self.error: raise RuntimeError(str(self.error))
            while True:
                try: self.lines.get_nowait()
                except queue.Empty: break
            self.log("command", {"port": self.path, "line": command or "(resynchronize console)"})
            self.port.write((command + "\n").encode("ascii"))
            if prefix is None: return []
            deadline, result = time.monotonic() + timeout, []
            while time.monotonic() < deadline:
                if self.error: raise RuntimeError(str(self.error))
                # Service only other channels. The caller must not recursively
                # request this UART while its command lock is held.
                if service: service()
                try: line = self.lines.get(timeout=min(0.1, max(0, deadline - time.monotonic())))
                except queue.Empty: continue
                result.append(line)
                if line.startswith(("KMSGEN ERROR", "KMSBENCH ERROR", "KMSROUTE ERROR", "KMSTEST ERROR", "KMSISO ERROR")):
                    raise RuntimeError(line)
                if line.startswith(prefix): return result
            observed = "; ".join(result[-3:]) or "no UART data received"
            raise TimeoutError(f"{self.path}: no reply to {command!r} at 115200 baud; {observed}")

    def close(self):
        self.closed.set()
        self.thread.join(timeout=1)
        self.port.close()


def serial_ports():
    from serial.tools import list_ports
    paths = {}
    if os.path.isdir("/dev/serial/by-id"):
        for name in sorted(os.listdir("/dev/serial/by-id")):
            path = "/dev/serial/by-id/" + name
            paths.setdefault(os.path.realpath(path), path)
    return [paths.get(p.device, p.device) for p in list_ports.comports() if p.vid is not None]


@dataclass(frozen=True)
class FirmwareIdentity:
    role: str
    identity: str
    boot: str = ""
    trace: bool = False


def read_identity(channel, attempts=4, timeout=1.5):
    """Retry only the read-only handshake, keeping one UART connection open.

    Opening a bridge can lose the first command during board startup. A blank
    line also terminates any incomplete console command from a previous client.
    No reset, stop, routing or pairing command is sent before identity is known.
    """
    for attempt in range(attempts):
        channel.log("probe", {"port": channel.path, "line": f"Identity probe {attempt + 1}/{attempts}"})
        channel.request("")
        try:
            line = channel.request("bench info", ("KMSGEN INFO ", "KMSBENCH INFO "), timeout=timeout)[-1]
        except (TimeoutError, RuntimeError) as error:
            if isinstance(error, RuntimeError) and str(error) != "KMSGEN ERROR invalid command": raise
            if attempt + 1 == attempts: raise
            continue
        fields = line.split()
        if fields[2] != "1": raise ValueError(f"Unsupported test protocol: {line}")
        if (fields[0] == "KMSGEN" and len(fields) in (6,7) and fields[4] == "54"
                and (len(fields)==6 or fields[6]=="trace=1")):
            result = FirmwareIdentity("generator", fields[3].upper(), trace=len(fields)==7)
        elif (fields[0] == "KMSBENCH" and len(fields) == 7 and fields[3] == "master" and
              re.fullmatch(r"[0-9a-fA-F]{8}", fields[5]) and fields[6] in tuple(map(str, range(8)))):
            result = FirmwareIdentity("master", fields[4].upper(), fields[5].lower())
        else: raise ValueError(f"Unexpected firmware identity: {line}")
        if not re.fullmatch(r"[0-9A-F]{12}", result.identity):
            raise ValueError(f"Invalid chip identity: {line}")
        return result
    raise ValueError("Identity attempts must be positive")


def discover_serial(log=lambda *args: None, cancel=None):
    found, problems = [], []
    for path in serial_ports():
        if cancel is not None and cancel.is_set(): break
        channel = None
        try:
            log("probe", {"port": path, "line": "Opening UART with BOOT/RESET deasserted"})
            channel = SerialChannel(path, log)
            info = read_identity(channel)
            found.append((info.role, Device(info.identity, info.role.title(), path)))
        except Exception as error:
            detail = str(error)
            problems.append(detail if detail.startswith(path + ":") else f"{path}: {detail}")
        finally:
            if channel: channel.close()
    return found, problems


def peers(channel):
    lines = channel.request("slaves", "KMSPEERS END", timeout=8)
    serials, boot, selected = {}, {}, None
    for line in lines:
        fields = line.split()
        if line.startswith("KMSPEERUSB "): serials[int(fields[1])] = fields[2].upper()
        elif line.startswith("KMSPEER "): boot[int(fields[1])] = fields[2]
        elif line.startswith("KMSROUTE CURRENT "): selected = int(fields[2])
    return serials, boot, selected, lines


@dataclass
class ReceiverNodes:
    identity: str
    hid: dict = field(default_factory=dict)
    events: list = field(default_factory=list)


def receiver_nodes(pid=b"4005"):
    import pyudev
    context, result = pyudev.Context(), {}
    for subsystem in ("hidraw", "input"):
        for node in context.list_devices(subsystem=subsystem):
            if not node.device_node or (subsystem == "input" and not node.sys_name.startswith("event")): continue
            usb = node.find_parent("usb", "usb_device")
            interface = node.find_parent("usb", "usb_interface")
            if usb is None or interface is None: continue
            if usb.attributes.get("idVendor") != b"303a" or usb.attributes.get("idProduct") != pid: continue
            identity = usb.attributes.get("serial", b"").decode().upper()
            if not identity: continue
            entry = result.setdefault(identity, ReceiverNodes(identity))
            if subsystem == "hidraw":
                number = int(interface.attributes.get("bInterfaceNumber"), 16)
                if number in entry.hid: raise RuntimeError(f"Ambiguous HID interface for {identity}")
                entry.hid[number] = node.device_node
            else: entry.events.append(node.device_node)
    return result


class Capture:
    def __init__(self, devices, consume, log=lambda *args: None, direct=False):
        self.devices, self.consume, self.log = devices, consume, log
        self.selector = selectors.DefaultSelector()
        self.guard_selector = selectors.DefaultSelector()
        self.handles, self.guards = {}, []
        self.stop = threading.Event()
        self.error = None
        self.thread = None
        self.guard_thread = None
        self.direct = direct

    def open(self):
        from evdev import InputDevice
        nodes = receiver_nodes(b"4006" if self.direct else b"4005")
        try:
            for device in self.devices:
                entry = nodes.get(device.identity.upper())
                if not entry or set(entry.hid) != ({0} if self.direct else {0, 1, 2}) or len(entry.events) < (1 if self.direct else 2):
                    expected = "generator native USB" if self.direct else "all three slave HID interfaces"
                    raise RuntimeError(f"{device.name}: {expected} and its input nodes must be present; check wiring/permissions")
                for path in entry.events:
                    guard = InputDevice(path)
                    self.guards.append(guard)
                    guard.grab()
                    self.guard_selector.register(guard.fd, selectors.EVENT_READ, guard)
                for interface, path in entry.hid.items():
                    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
                    self.handles[device.slot, interface] = fd
                    self.selector.register(fd, selectors.EVENT_READ, (device.slot, interface))
            self.thread = threading.Thread(target=self._read, daemon=True)
            self.guard_thread = threading.Thread(target=self._read_guards, daemon=True)
            self.guard_thread.start()
            self.thread.start()
        except Exception:
            self.close(); raise

    def _read(self):
        try:
            while not self.stop.is_set():
                for key, _ in self.selector.select(0.05):
                    kind, instance = key.data
                    # Short batches leave space in sibling hidraw queues while
                    # logging/verification or host scheduling delays this reader.
                    for _ in range(8):
                        if self.stop.is_set(): break
                        try: data = os.read(key.fd, 256)
                        except BlockingIOError: break
                        if not data: raise RuntimeError(f"Slave {kind}: USB disconnected")
                        now = time.monotonic_ns()
                        self.log("hid", {"slot": kind, "interface": instance, "data": data.hex(), "received_ns": now})
                        self.consume(kind, instance, data, now)
        except Exception as error:
            if self.error is None: self.error = error

    def _read_guards(self):
        # Input grabs protect the desktop. Drain their queues independently of
        # HID decoding, stream verification, trace downloads and evidence writes.
        try:
            while not self.stop.is_set():
                for key, _ in self.guard_selector.select(0.05):
                    try:
                        for event in key.data.read():
                            if event.type == 0 and event.code == 3:
                                # This evdev client only holds the desktop grab;
                                # it does not reconstruct state or verify input.
                                # Keep draining after SYN_DROPPED. The independent
                                # hidraw stream and device checkpoints prove loss.
                                self.log("capture-warning", {"warning": "desktop_event_queue_overrun",
                                                            "device": key.data.path})
                    except BlockingIOError:
                        continue
        except Exception as error:
            if self.error is None: self.error = error

    def feature(self, slot, op, value=0):
        import fcntl
        fd = self.handles[slot, 2]
        # Linux hidraw feature ioctl includes the report ID at byte zero.
        data = bytearray(129)
        data[0] = 126
        struct.pack_into("<4sII", data, 1, b"KMT1", op, value)
        fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord("H") << 8) | 0x06, data)

    def clipboard_feature(self, slot, report, payload=None):
        """Desktop/clipboard feature traffic on the already owned receiver FD."""
        import fcntl
        if report not in (32, 34, 35) or (payload is not None and len(payload) not in (64, 192)):
            raise ValueError("Invalid clipboard stress feature report")
        data = bytearray(193); data[0] = report
        if payload is not None: data[1:1 + len(payload)] = payload
        operation = 0x07 if payload is None else 0x06
        fcntl.ioctl(self.handles[slot, 2], (3 << 30) | (len(data) << 16) | (ord("H") << 8) | operation, data, True)
        return bytes(data[1:])

    def page(self, slot, index):
        import fcntl
        self.feature(slot, 4, index)
        data = bytearray(129); data[0] = 126
        fcntl.ioctl(self.handles[slot, 2], (3 << 30) | (len(data) << 16) | (ord("H") << 8) | 0x07, data, True)
        return trace_page(bytes(data[1:]))

    def leds(self, slot, mask):
        os.write(self.handles[slot, 0], bytes([0, mask & 31]))

    def traces(self, slot):
        self.feature(slot, 2)
        first = self.page(slot, 0)
        result = {**first, "records": []}
        for index in range(min(first["records_total"], 256)):
            page = first if index == 0 else self.page(slot, index)
            if page["boot"] != first["boot"] or page["run"] != first["run"] or page["records_total"] != first["records_total"]:
                raise RuntimeError(f"Slave {slot}: trace changed/reset during download")
            if page["record"]: result["records"].append(page["record"])
        result.pop("record", None)
        result["summary"] = self.page(slot, 0xffffffff)["summary"]
        return result

    def close(self):
        self.stop.set()
        if self.thread: self.thread.join(timeout=2)
        if self.guard_thread: self.guard_thread.join(timeout=2)
        self.selector.close()
        self.guard_selector.close()
        for fd in self.handles.values(): os.close(fd)
        self.handles.clear()
        for guard in self.guards:
            try: guard.ungrab()
            except OSError: pass
            guard.close()
        self.guards.clear()
