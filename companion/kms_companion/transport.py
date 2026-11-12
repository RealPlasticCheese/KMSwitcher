"""One HID owner. Command reports use control transfers, status polling stays local."""
import secrets
import struct
import sys
import time
from .protocol import (CONFIG_TOLERANCES, DEFAULT_JOIN_TOLERANCE, ERRORS, Message, Op,
                       decode_config, encode_config, join_tolerances, read_words, words)

VID, PID = 0x303a, 0x4005
COMMAND, STATUS = 32, 33
FEATURE_BYTES = 193
STREAM_STATUS_TAG = 0x53000000
PEER_JOIN_PREVIEW = 0x80000000
PEER_ALL_ROUTES = 0x40000000


class DeviceError(RuntimeError):
    pass


class RejectedCommand(DeviceError):
    pass


def _hid_backend():
    # The Linux libusb backend omits usages and needs USB-bus access. hidraw
    # uses our udev ACLs and keeps the kernel's keyboard/mouse drivers attached.
    if sys.platform == "linux":
        import hidraw as hid
    else:
        import hid
    return hid


def devices():
    hid = _hid_backend()
    return [d for d in hid.enumerate(VID, PID)
            if d.get("usage_page") == 0xff00 and d.get("usage") == 2]


class Receiver:
    def __init__(self, serial=None):
        hid = _hid_backend()
        found = devices()
        if serial:
            found = [d for d in found if d.get("serial_number") == serial]
        if len(found) != 1:
            raise DeviceError("Connect exactly one desktop-capable receiver, or select --serial; old firmware needs updating")
        self.info = found[0]
        self.device = hid.device()
        self.device.open_path(self.info["path"])
        self.session = secrets.randbits(32) or 1
        self.sequence = secrets.randbits(30)
        self.master_boot = 0
        self.peer_geometry = {}
        self.peer_joins = {}
        self.stream_supported = None
        self.stream_profile = None
        self.stream_state = {}
        self.diagnostics = False
        try:
            initial = bytes(self.device.get_feature_report(STATUS, FEATURE_BYTES))
            if len(initial) >= 65:
                self.diagnostics = Message.decode(initial[1:65]).data[45] == 1
            if self.diagnostics:
                self.request(Op.LOCAL_HELLO, timeout=.3)
                try:
                    self.request(Op.HELLO, timeout=.2)
                except DeviceError:
                    pass  # Local diagnostics remain usable without the S3 radio link.
            else:
                self.request(Op.HELLO)
        except Exception:
            self.close()
            raise

    def close(self):
        self.device.close()

    def request(self, op, data=b"", timeout=2.0):
        self.sequence = (self.sequence + 1) & 0xffffffff or 1
        request = Message(op, self.sequence, self.session, data)
        end, retry = time.monotonic() + timeout, 0
        while time.monotonic() < end:
            now = time.monotonic()
            if now >= retry:
                self.device.send_feature_report(bytes([COMMAND]) + request.encode())
                retry = now + .15
            raw = bytes(self.device.get_feature_report(COMMAND, FEATURE_BYTES))
            if len(raw) >= 65:
                reply = Message.decode(raw[1:65])
                if reply.session == self.session and reply.id == request.id and reply.op == op:
                    if reply.result:
                        raise RejectedCommand(ERRORS[reply.result])
                    return reply
            time.sleep(.005)
        raise DeviceError("Master did not acknowledge command")

    def status(self):
        raw = bytes(self.device.get_feature_report(STATUS, FEATURE_BYTES))
        if len(raw) < 65:
            raise DeviceError("Short HID status report")
        report = Message.decode(raw[1:65])
        if report.session != self.session:
            raise DeviceError("Receiver connection changed; reconnecting")
        d = report.data
        result = dict(slot=d[0], active=bool(d[1]), held=bool(d[2]), usb_ready=bool(d[3]),
                    boot=read_words(d, 1, 4)[0], master_boot=read_words(d, 1, 8)[0], diagnostics=d[45] == 1,
                    motion_sequence=read_words(d, 1, 12)[0], dx=struct.unpack_from("<i", d, 16)[0],
                    dy=struct.unpack_from("<i", d, 20)[0], event=read_words(d, 1, 24)[0],
                    event_x=struct.unpack_from("<i", d, 28)[0], event_y=struct.unpack_from("<i", d, 32)[0],
                    event_geometry=read_words(d, 1, 36)[0], generation=read_words(d, 1, 40)[0], clipboard=d[44] == 1,
                    power_supported=bool(d[47] & 0x80), power_state=power_name(d[46]), power_fresh=bool(d[47] & 1))

        if report.id & 0xff000000 == STREAM_STATUS_TAG:
            free, incoming, effective = (report.id >> 8) & 255, (report.id >> 16) & 255, report.id & 255
            if free > 4 or incoming > 4 or effective not in (0, 1, 2, 255):
                raise DeviceError("Invalid receiver stream status")
            result["stream"] = dict(free=free, incoming=incoming, effective=effective, slot=d[0])
        return result

    def power(self, state):
        self.request(Op.POWER, words(state), timeout=.2)

    def _stream_profile(self, profile):
        from kms_transport import PROFILES
        import zlib
        requested = PROFILES.index(profile)
        if self.stream_profile != requested:
            settings = bytearray(32); settings[:5] = b"KMS2\x01"; settings[7] = requested
            struct.pack_into("<I", settings, 12, zlib.crc32(settings[:12]))
            self.device.send_feature_report(bytes([36]) + settings)
            self.stream_profile = requested

    def stream_capacity(self, profile, snapshot=None):
        """Called by the HID owner; unknown reports fall back to one legacy send."""
        from kms_transport import PROFILES
        import zlib
        if snapshot is not None:
            self.stream_supported = True
            effective = snapshot["effective"]
            self.stream_state = dict(requested=profile, free=snapshot["free"], slots=6,
                links={str(snapshot["slot"]): PROFILES[effective] if effective < len(PROFILES) else "legacy"})
            self._stream_profile(profile)
            return snapshot["free"]
        if self.stream_supported is False:
            return 1
        try:
            raw = bytes(self.device.get_feature_report(36, FEATURE_BYTES))
            data = raw[1:33]
            if (len(data) != 32 or data[:5] != b"KMS2\x01" or data[7] >= len(PROFILES) or
                    struct.unpack_from("<I", data, 12)[0] != zlib.crc32(data[:12])):
                self.stream_supported = False
                return 1
            self.stream_supported = True
            self.stream_state = dict(requested=PROFILES[data[7]], free=data[5], slots=data[8],
                links={str(slot + 1): PROFILES[data[16 + slot]] if data[23] & (1 << slot) else "legacy"
                       for slot in range(7) if data[24] & (1 << slot) and data[16 + slot] < len(PROFILES)})
            self._stream_profile(profile)
            return min(data[5], 4)
        except OSError:
            if self.stream_supported:
                raise  # A failed read on a negotiated channel is a connection error.
            self.stream_supported = False
            return 1

    def clipboard_send(self, packet):
        from .clipboard_protocol import SEND_REPORT
        self.device.send_feature_report(bytes([SEND_REPORT]) + packet.encode())

    def clipboard_receive(self):
        from .clipboard_protocol import Packet, RECEIVE_REPORT, REPORT_BYTES
        raw = bytes(self.device.get_feature_report(RECEIVE_REPORT, REPORT_BYTES + 1))
        if len(raw) != REPORT_BYTES + 1:
            raise DeviceError("Clipboard channel requires updated receiver firmware")
        return Packet.decode(raw[1:]) if any(raw[1:]) else None

    def clipboard_peer(self, slot):
        d = self.request(Op.PEER, words(slot, 0xffffffff), timeout=.12).data
        return dict(session=read_words(d, 1, 8)[0], age_ms=read_words(d, 1, 12)[0])

    def heartbeat(self, enabled, ready):
        if self.diagnostics and not self.status()['master_boot']:
            self.request(Op.HELLO, timeout=.15)
        data = self.request(Op.HEARTBEAT, words(int(enabled) | int(ready) << 1),
                            timeout=.2 if self.diagnostics else 2).data
        self.edge_departure = read_words(data, 1, 16)[0]
        return read_words(data, 4)

    def screens(self, rects, version):
        if not 1 <= len(rects) <= 16:
            raise DeviceError("This protocol supports 1..16 active monitors per station")
        self.request(Op.SCREENS_BEGIN, words(version, len(rects)))
        for index, r in enumerate(rects):
            self.request(Op.SCREEN, words(version, index, r.x, r.y, r.width, r.height))
        self.request(Op.SCREENS_COMMIT, words(version))

    def config(self):
        for _ in range(3):
            data, revision, tolerances = bytearray(), None, None
            for offset in (0, 32, 64):
                r = self.request(Op.CONFIG_READ, words(offset, CONFIG_TOLERANCES))
                current = read_words(r.data, 1)[0]
                if revision is not None and current != revision:
                    break
                revision = current
                data.extend(r.data[8:40])
                if offset == 0 and r.data[7] == 1:
                    tolerances = read_words(r.data, 2, 40)
            if len(data) == 96:
                config = decode_config(data)
                self.tolerance_supported = tolerances is not None
                if tolerances is not None:
                    config.update(gap_tolerance=tolerances[0], overlap_tolerance=tolerances[1])
                return config, revision
        raise DeviceError("Configuration changed during read; retry")

    def save_config(self, config, revision, *, refresh_joins=False):
        blob = encode_config(config)
        gap, overlap = join_tolerances(config)
        if not getattr(self, "tolerance_supported", True) and (gap, overlap) != (DEFAULT_JOIN_TOLERANCE,) * 2:
            raise DeviceError("Update the controller firmware to save custom join tolerances")
        begin = self.request(Op.CONFIG_BEGIN, words(revision, CONFIG_TOLERANCES, gap, overlap, int(refresh_joins)))
        if read_words(begin.data, 1, 8)[0] != CONFIG_TOLERANCES and (gap, overlap) != (DEFAULT_JOIN_TOLERANCE,) * 2:
            raise DeviceError("Update the controller firmware to save custom join tolerances")
        token = read_words(begin.data, 1, 4)[0]
        for offset in (0, 32, 64):
            self.request(Op.CONFIG_CHUNK, words(token, offset) + blob[offset:offset + 32])
        return read_words(self.request(Op.CONFIG_COMMIT, words(token)).data, 1)[0]

    def peers(self, *, include_joins=False):
        result, snapshot = [], None
        flags = (PEER_ALL_ROUTES,) if include_joins else ()
        for slot in range(1, 8):
            d = self.request(Op.PEER, words(slot, 0xffffffff, *flags)).data
            if include_joins and d[39] & 2:
                current = read_words(d, 1)[0], read_words(d, 1, 44)[0]
                if snapshot is not None and snapshot != current:
                    raise RejectedCommand("Station display layout changed while reading; retry")
                snapshot = current
            peer = dict(slot=slot, eligible=bool(d[4]), screens=[], enabled=bool(d[6]), ready=bool(d[7]),
                        session=read_words(d, 1, 8)[0], age_ms=read_words(d, 1, 12)[0])
            key = peer["session"], read_words(d, 1, 32)[0], d[5]
            cached_key, screens = self.peer_geometry.get(slot, (None, []))
            if key != cached_key:
                screens = []
                for index in range(d[5]):
                    rect = self.request(Op.PEER, words(slot, index)).data
                    if (read_words(rect, 1, 8)[0], read_words(rect, 1, 32)[0], rect[5]) != key:
                        raise RejectedCommand("Station display layout changed while reading; retry")
                    x, y, w, h = struct.unpack_from("<iiii", rect, 16)
                    screens.append(dict(x=x, y=y, width=w, height=h))
                self.peer_geometry[slot] = key, screens
            peer["screens"] = screens
            peer.update(power_supported=d[38] == 1, power_state=power_name(d[36]), power_fresh=d[37] == 1,
                        power_age_ms=read_words(d, 1, 40)[0] if d[38] == 1 and d[36] else None)
            if include_joins and d[39]:
                revision = read_words(d, 1)[0]
                topology = read_words(d, 1, 44)[0]
                all_routes = bool(d[39] & 2)
                key = revision, topology, all_routes
                cached_key, lines = self.peer_joins.get(slot, (None, []))
                if not (d[39] & 1):
                    lines = []
                elif key != cached_key:
                    lines = self.route_preview(slot, revision, topology, all_routes)
                    self.peer_joins[slot] = key, lines
                field = "crossings" if all_routes else "remembered_edges"
                peer.update({field: lines, "layout_revision": revision})
            result.append(peer)
        return result

    def route_preview(self, slot, revision, topology, all_routes):
        flag = PEER_JOIN_PREVIEW | (PEER_ALL_ROUTES if all_routes else 0)
        cursor, lines = 0, []
        while True:
            d = self.request(Op.PEER, words(slot, flag | cursor)).data
            if (read_words(d, 1)[0], read_words(d, 1, 44)[0]) != (revision, topology):
                raise RejectedCommand("Station display layout changed while reading; retry")
            if not d[4]:
                break
            next_cursor = read_words(d, 1, 32)[0]
            if not cursor < next_cursor < PEER_ALL_ROUTES:
                raise RejectedCommand("Invalid remembered edge preview")
            line = struct.unpack_from("<iiii", d, 16)
            if all_routes:
                kind, target, direction = d[5:8]
                if kind not in (1, 2, 3, 4) or not 1 <= target <= 7 or direction > 3:
                    raise RejectedCommand("Invalid crossing preview")
                lines.append(dict(line=line, kind=kind, target=target, direction=direction))
            else:
                lines.append(line)
            cursor = next_cursor
        return lines


def power_name(value):
    from .power import PowerState
    try:
        return PowerState(value).name.lower()
    except ValueError:
        return "unknown"
