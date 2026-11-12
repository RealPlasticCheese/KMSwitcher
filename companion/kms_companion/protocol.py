"""Explicit little-endian codecs shared with kms_desktop.h; no native struct layout."""
from dataclasses import dataclass
from enum import IntEnum
import struct
import zlib


class Op(IntEnum):
    HELLO = 1
    HEARTBEAT = 2
    SCREENS_BEGIN = 3
    SCREEN = 4
    SCREENS_COMMIT = 5
    CONFIG_READ = 6
    CONFIG_BEGIN = 7
    CONFIG_CHUNK = 8
    CONFIG_COMMIT = 9
    PEER = 10
    CROSS = 11
    PREPARE = 12
    READY = 13
    SELECT = 14
    PREFERENCES_GET = 15
    PREFERENCES_SET = 16
    STATUS = 17
    LOCAL_HELLO = 18
    LOG_BEGIN = 19
    LOG_READ = 20
    LOG_END = 21
    RECOVERY_STATUS = 22
    POWER = 23
    CHANNEL_GET = 24
    CHANNEL_SET = 25
    CHANNEL_RESUME = 26
    DEVICE_CONTROL = 27
    USB_SNAPSHOT = 28
    USB_DEVICE = 29
    USB_INTERFACE = 30
    USB_CHANGE = 31
    REMOTE_REQUEST = 32
    REMOTE_READ = 33
    PREFERENCES_DEFAULT = 34
    CHANNEL_RECOVERY_GET = 35
    CHANNEL_RECOVERY_SET = 36


ERRORS = ("ok", "invalid request", "stale session", "busy", "station unavailable",
          "storage failure", "unsupported firmware", "configuration changed; reload before saving")


@dataclass(frozen=True)
class Message:
    op: int
    id: int = 0
    session: int = 0
    data: bytes = b""
    result: int = 0

    def encode(self):
        if len(self.data) > 48:
            raise ValueError("Desktop payload exceeds 48 bytes")
        body = struct.pack("<BBBBII48s", ord("K"), 1, self.op, self.result,
                           self.id, self.session, self.data)
        return body + struct.pack("<I", zlib.crc32(body))

    @classmethod
    def decode(cls, data):
        if len(data) != 64 or data[:2] != b"K\x01" or zlib.crc32(data[:60]) != struct.unpack_from("<I", data, 60)[0]:
            raise ValueError("Invalid desktop report or incompatible firmware")
        _, _, op, result, ident, session, payload = struct.unpack("<BBBBII48s", data[:60])
        if op not in Op._value2member_map_ or result >= len(ERRORS):
            raise ValueError("Unknown desktop operation/result")
        return cls(op, ident, session, payload, result)


def words(*values):
    return struct.pack("<" + "I" * len(values), *(v & 0xffffffff for v in values))


def read_words(data, count, offset=0):
    return struct.unpack_from("<" + "I" * count, data, offset)


DEFAULT_JOIN_TOLERANCE = 8
MAX_JOIN_TOLERANCE = 0xffffffff
CONFIG_TOLERANCES = 0x314c4f54


def join_tolerances(config):
    values = tuple(config.get(key, DEFAULT_JOIN_TOLERANCE) for key in ("gap_tolerance", "overlap_tolerance"))
    if any(type(value) is not int or not 0 <= value <= MAX_JOIN_TOLERANCE for value in values):
        raise ValueError("Join tolerances must be whole numbers from 0 to 4,294,967,295")
    return values


def default_config():
    return {"schema": 1, "enabled": True, "delay_ms": 100, "corner": 8,
            "gap_tolerance": DEFAULT_JOIN_TOLERANCE, "overlap_tolerance": DEFAULT_JOIN_TOLERANCE,
            "stations": [{"slot": i, "configured": False, "x": 0, "y": 0, "width": 0, "height": 0} for i in range(1, 8)]}


def encode_config(config):
    if config.get("schema") != 1 or type(config.get("enabled")) is not bool:
        raise ValueError("Expected configuration schema 1 and boolean enabled")
    delay, corner = config.get("delay_ms"), config.get("corner")
    if type(delay) is not int or not 0 <= delay <= 2000 or type(corner) is not int or not 0 <= corner <= 64:
        raise ValueError("delay_ms must be 0..2000; corner must be 0..64")
    join_tolerances(config)
    stations = config.get("stations", [])
    if len(stations) != 7 or sorted(s.get("slot", 0) for s in stations) != list(range(1, 8)):
        raise ValueError("Configuration must contain slots 1..7 exactly once")
    data = bytearray(96)
    struct.pack_into("<4sBBH", data, 0, b"KDL1", config["enabled"], corner, delay)
    for station in stations:
        x, y, configured = station.get("x"), station.get("y"), station.get("configured")
        if type(x) is not int or type(y) is not int or not -100000 <= x <= 100000 or not -100000 <= y <= 100000 or type(configured) is not bool:
            raise ValueError("Station coordinates must be integers within +/-100000; configured must be boolean")
        width, height = station.get("width", 0), station.get("height", 0)
        if type(width) is not int or type(height) is not int or not 0 <= width <= 32767 or not 0 <= height <= 32767:
            raise ValueError("Station width/height must be integers from 0 to 32767")
        struct.pack_into("<iiI", data, 8 + (station["slot"] - 1) * 12, x, y, int(configured) | width << 1 | height << 16)
    struct.pack_into("<I", data, 92, zlib.crc32(data[:92]))
    return bytes(data)


def decode_config(data):
    if len(data) != 96 or data[:4] != b"KDL1" or zlib.crc32(data[:92]) != struct.unpack_from("<I", data, 92)[0]:
        raise ValueError("Invalid layout configuration")
    config = default_config()
    enabled, corner, delay = struct.unpack_from("<BBH", data, 4)
    if enabled > 1:
        raise ValueError("Invalid enabled flag")
    config.update(enabled=bool(enabled), corner=corner, delay_ms=delay)
    for s in config["stations"]:
        x, y, flag = struct.unpack_from("<iiI", data, 8 + (s["slot"] - 1) * 12)
        if flag & 0x80000000:
            raise ValueError("Invalid station flag")
        s.update(x=x, y=y, configured=bool(flag & 1), width=(flag >> 1) & 32767, height=(flag >> 16) & 32767)
    encode_config(config)
    return config
