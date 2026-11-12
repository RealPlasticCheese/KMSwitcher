"""Bounded clipboard packets; the same operations travel over radio and encrypted TCP."""
from dataclasses import dataclass, replace
from enum import IntEnum
import hashlib
import hmac
import ipaddress
import socket
import struct
import zlib


class Kind(IntEnum):
    QUERY = 1
    INFO = 2
    BEGIN = 3
    DATA = 4
    END = 5
    ACK = 6
    CANCEL = 7
    ERROR = 8


MODES = ("dongle", "network-preferred", "network")
MODE_LABELS = ("Dongle only", "Network preferred, fallback to dongle", "Network only")
HEADER = struct.Struct("<4sBBBBIIIII")
DATA_BYTES = 160
QUERY_MACHINE_ID = 1
REPORT_BYTES = 192
SEND_REPORT, RECEIVE_REPORT = 34, 35
LAN_HEADER = struct.Struct("<4sBBII16s")
MAX_RECORD = 65536 + HEADER.size + 28


def transports(local, remote):
    if local not in MODES or remote not in MODES:
        raise ValueError("Unknown clipboard transport preference")
    network = local != "dongle" and remote != "dongle"
    dongle = local != "network" and remote != "network"
    return (["network"] if network else []) + (["dongle"] if dongle else [])


@dataclass(frozen=True)
class Packet:
    kind: int
    source: int
    target: int
    source_session: int
    target_session: int
    sequence: int
    transfer: int = 0
    offset: int = 0
    data: bytes = b""

    def encode(self, radio=True):
        if (self.kind not in Kind._value2member_map_ or not 1 <= self.source <= 7 or
                not 1 <= self.target <= 7 or self.source == self.target or
                not self.source_session or not self.target_session or not self.sequence or
                len(self.data) > (DATA_BYTES if radio else 65536)):
            raise ValueError("Invalid clipboard packet")
        # TCP records use a four-byte length, not the radio length byte.
        body = HEADER.pack(b"KCB1", self.kind, self.source, self.target,
                           len(self.data) if radio else 0, self.source_session, self.target_session,
                           self.sequence, self.transfer, self.offset) + self.data
        if not radio:
            return body
        body = body.ljust(REPORT_BYTES - 4, b"\0")
        return body + struct.pack("<I", zlib.crc32(body))

    @classmethod
    def decode(cls, data, radio=True):
        if len(data) < HEADER.size:
            raise ValueError("Short clipboard packet")
        magic, kind, source, target, length, ss, ts, seq, transfer, offset = HEADER.unpack_from(data)
        if magic != b"KCB1":
            raise ValueError("Unknown clipboard protocol")
        if radio:
            if (len(data) != REPORT_BYTES or length > DATA_BYTES or
                    zlib.crc32(data[:-4]) != struct.unpack_from("<I", data, REPORT_BYTES - 4)[0]):
                raise ValueError("Damaged clipboard packet")
            payload = data[HEADER.size:HEADER.size + length]
        else:
            payload = data[HEADER.size:]
        result = cls(kind, source, target, ss, ts, seq, transfer, offset, payload)
        if result.encode(radio) != data:
            raise ValueError("Noncanonical clipboard packet")
        return result

    def reply(self, kind, data=b"", offset=0):
        return replace(self, kind=kind, source=self.target, target=self.source,
                       source_session=self.target_session, target_session=self.source_session,
                       data=data, offset=offset)


@dataclass(frozen=True)
class Advertisement:
    mode: str
    ready: bool
    port: int
    token: bytes
    name: str
    addresses: tuple
    machine_id: bytes = b""

    def encode(self):
        name = self.name.encode("utf-8")[:48]
        # Avoid splitting a multibyte host name.
        name = name.decode("utf-8", errors="ignore").encode("utf-8")
        ips = [ipaddress.IPv4Address(a).packed for a in self.addresses[:4]]
        if (self.mode not in MODES or len(self.token) != 32 or not 0 <= self.port <= 65535 or
                len(self.machine_id) not in (0, 16)):
            raise ValueError("Invalid companion advertisement")
        return struct.pack("<BBH32s48sB", MODES.index(self.mode), bool(self.ready), self.port,
                           self.token, name, len(ips)) + b"".join(ips) + self.machine_id

    @classmethod
    def decode(cls, data):
        if len(data) < 85:
            raise ValueError("Short companion advertisement")
        mode, ready, port, token, name, count = struct.unpack_from("<BBH32s48sB", data)
        end = 85 + count * 4
        if mode >= len(MODES) or ready > 1 or count > 4 or len(data) - end not in (0, 16):
            raise ValueError("Invalid companion advertisement")
        addresses = tuple(str(ipaddress.IPv4Address(data[i:i + 4])) for i in range(85, end, 4))
        return cls(MODES[mode], bool(ready), port, token, name.rstrip(b"\0").decode("utf-8"), addresses, data[end:])


def local_addresses():
    addresses = set()
    try:
        addresses.update(info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            # Route lookup only: connect on UDP sends no packet and works without internet access.
            probe.connect(("192.0.2.1", 9))
            addresses.add(probe.getsockname()[0])
    except OSError:
        pass
    return tuple(sorted(a for a in addresses if not ipaddress.ip_address(a).is_loopback and a != "0.0.0.0"))[:4]


def network_key(receiver_token, sender_token, header):
    return hmac.digest(receiver_token, b"KMSwitcher clipboard LAN v1\0" + sender_token + header, hashlib.sha256)


def receive_exact(sock, count):
    chunks = bytearray()
    while len(chunks) < count:
        chunk = sock.recv(count - len(chunks))
        if not chunk:
            raise ConnectionError("Clipboard network connection closed")
        chunks.extend(chunk)
    return bytes(chunks)


class SecureRecords:
    """Per-connection directional keys and ordered AES-GCM records prevent reflection/replay."""
    def __init__(self, sock, key, header, client):
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        self.sock, self.header = sock, header
        self.tx = AESGCM(hmac.digest(key, b"client" if client else b"server", "sha256"))
        self.rx = AESGCM(hmac.digest(key, b"server" if client else b"client", "sha256"))
        self.sent = self.received = 0
        # Unique connection salt makes reconnects safe with the same session key.
        import secrets
        self.salt = secrets.token_bytes(4)
        self.remote_salt = None

    def send(self, packet):
        nonce = self.salt + self.sent.to_bytes(8, "little")
        ciphertext = nonce + self.tx.encrypt(nonce, packet.encode(False), self.header)
        self.sock.sendall(struct.pack("<I", len(ciphertext)) + ciphertext)
        self.sent += 1

    def receive(self):
        from cryptography.exceptions import InvalidTag
        size = struct.unpack("<I", receive_exact(self.sock, 4))[0]
        if not 28 + HEADER.size <= size <= MAX_RECORD:
            raise ValueError("Invalid encrypted record size")
        record = receive_exact(self.sock, size)
        nonce = record[:12]
        if self.remote_salt is not None and nonce[:4] != self.remote_salt:
            raise ValueError("Changed network record salt")
        if int.from_bytes(nonce[4:], "little") != self.received:
            raise ValueError("Replayed or reordered network record")
        try:
            packet = Packet.decode(self.rx.decrypt(nonce, record[12:], self.header), False)
        except InvalidTag as error:
            raise ValueError("Network clipboard authentication failed") from error
        self.remote_salt = nonce[:4]
        self.received += 1
        return packet
