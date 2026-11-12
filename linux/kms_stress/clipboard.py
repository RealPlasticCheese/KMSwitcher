"""Synthetic dongle-only file traffic, including an interrupted transfer.

Owns no sockets, files or desktop clipboard. Uses the published feature/radio
protocol through Capture's existing receiver handles; the input oracle stays live.
"""
import hashlib
import secrets
import struct
import time
import zlib


def desktop(op, sequence, session, ready=True):
    body = struct.pack("<BBBBII48s", ord("K"), 1, op, 0, sequence, session,
                       struct.pack("<I", 2 if ready else 0) if op == 2 else b"")
    return body + struct.pack("<I", zlib.crc32(body))


def packet(kind, source, target, sessions, sequence, transfer, offset=0, data=b""):
    body = struct.pack("<4sBBBBIIIII", b"KCB1", kind, source, target, len(data),
                       sessions[source], sessions[target], sequence, transfer, offset) + data
    body = body.ljust(188, b"\0")
    return body + struct.pack("<I", zlib.crc32(body))


def decode(raw):
    if not any(raw): return None
    if len(raw) != 192 or raw[:4] != b"KCB1" or zlib.crc32(raw[:188]) != struct.unpack_from("<I", raw, 188)[0]:
        raise RuntimeError("Clipboard stress: corrupt radio/USB payload")
    _, kind, source, target, length, ss, ts, sequence, transfer, offset = struct.unpack_from("<4sBBBBIIIII", raw)
    if length > 160 or any(raw[28 + length:188]): raise RuntimeError("Clipboard stress: invalid packet length/padding")
    return dict(kind=kind, source=source, target=target, ss=ss, ts=ts, sequence=sequence,
                transfer=transfer, offset=offset, data=raw[28:28 + length])


class ClipboardTraffic:
    """Nonblocking pump: one small USB polling pass per input-monitor iteration."""
    def __init__(self, capture, source, target, log, clock=time.monotonic):
        if source == target: raise ValueError("Clipboard stress requires two receivers")
        self.capture, self.source, self.target, self.log, self.clock = capture, source, target, log, clock
        self.sessions = {s: secrets.randbelow(0xffffffff) + 1 for s in (source, target)}
        self.sequence = self.transfer = secrets.randbelow(0x7fffffff) + 1
        self.started = clock(); self.pending = {}; self.next_heartbeat = 0
        self.last_tick = self.started
        self.ready = set(); self.stage = "hello"; self.offset = 0
        self.content = bytes((i * 17 + 3) % 256 for i in range(1280))
        self.current = None; self.attempts = 0; self.sent_at = 0
        self.bytes_received = self.retries = self.duplicates = self.stale = 0
        self.completed = self.interrupted = False; self.recovery_since = None
        self.received = {}; self.abort = False

    def send(self, slot, report, raw):
        self.capture.clipboard_feature(slot, report, raw)

    def _desktop(self, slot, op):
        self.sequence += 1
        raw = desktop(op, self.sequence, self.sessions[slot])
        self.pending[slot] = (raw, self.clock(), self.sequence, op)
        self.send(slot, 32, raw)

    def _command(self, kind, data=b"", offset=0):
        self.sequence += 1
        self.current = packet(kind, self.source, self.target, self.sessions, self.sequence, self.transfer, offset, data)
        self.attempts = 0; self.sent_at = 0

    def _begin(self):
        self.offset = 0
        self._command(3, struct.pack("<I", len(self.content)) + hashlib.sha256(self.content).digest())

    def tick(self):
        now = self.clock()
        self.last_tick = now
        for slot in self.sessions:
            pending = self.pending.get(slot)
            if not pending:
                if slot not in self.ready or now >= self.next_heartbeat:
                    self._desktop(slot, 1 if slot not in self.ready else 2)
            else:
                raw, began, sequence, op = pending
                reply = self.capture.clipboard_feature(slot, 32)[:64]
                if (len(reply) == 64 and reply[:2] == b"K\x01" and
                    zlib.crc32(reply[:60]) == struct.unpack_from("<I", reply, 60)[0] and
                    reply[2] == op and struct.unpack_from("<II", reply, 4) == (sequence, self.sessions[slot])):
                    if reply[3]: raise RuntimeError(f"Clipboard stress: receiver {slot} rejected helper session")
                    del self.pending[slot]; self.ready.add(slot)
                elif now - began > 4:
                    raise RuntimeError(f"Clipboard stress: receiver {slot} helper heartbeat stalled")
        if now >= self.next_heartbeat and not self.pending: self.next_heartbeat = now + .7
        if len(self.ready) != 2: return
        if self.stage == "hello":
            self.stage = "transfer"; self._begin()
        for slot in (self.target, self.source):
            message = decode(self.capture.clipboard_feature(slot, 35))
            if message: self._receive(slot, message)
        if self.current and (not self.attempts or now - self.sent_at >= .8):
            if self.abort and self.attempts >= 2:
                self._command(7); self.stage = "cancel"; self.abort = False
            if self.attempts >= 5: raise RuntimeError("Clipboard stress: transfer progress stalled after five attempts")
            if self.attempts: self.retries += 1
            self.send(self.source, 34, self.current); self.attempts += 1; self.sent_at = now

    def _receive(self, slot, message):
        expected_source = self.source if slot == self.target else self.target
        if (message["source"], message["target"], message["ss"], message["ts"]) != (
            expected_source, slot, self.sessions[expected_source], self.sessions[slot]):
            self.stale += 1; return
        current = decode(self.current) if self.current else None
        if not current or (message["sequence"], message["transfer"]) != (current["sequence"], current["transfer"]):
            self.stale += 1; return
        if slot == self.target:
            if message != current: raise RuntimeError("Clipboard stress: relayed bytes changed")
            key = message["transfer"], message["sequence"]
            if key in self.received: self.duplicates += 1
            else:
                self.received[key] = True
                if message["kind"] == 4: self.bytes_received += len(message["data"])
            # Deliberately lose DATA receipts in the second transfer. This
            # exercises retries, cancellation and input after a stalled receive.
            if self.abort: return
            offset = message["offset"] + (len(message["data"]) if message["kind"] == 4 else 0)
            self.send(self.target, 34, packet(6, self.target, self.source, self.sessions,
                                             message["sequence"], self.transfer, offset))
            return
        if message["kind"] != 6: raise RuntimeError("Clipboard stress: invalid transfer acknowledgement")
        expected = current["offset"] + (len(current["data"]) if current["kind"] == 4 else 0)
        if message["offset"] != expected: raise RuntimeError("Clipboard stress: wrong acknowledged offset")
        if self.stage == "cancel":
            self.interrupted = True; self.current = None; self.stage = "recovery"; self.recovery_since = self.clock()
        elif current["kind"] == 5:
            self.completed = True; self.transfer += 1; self.stage = "interrupt"; self._begin()
        elif current["kind"] == 3 or current["kind"] == 4:
            self.offset = message["offset"]
            if self.stage == "interrupt": self.abort = True
            if self.offset < len(self.content): self._command(4, self.content[self.offset:self.offset + 160], self.offset)
            else: self._command(5, offset=self.offset)

    def finish(self):
        result = dict(source=self.source, target=self.target, bytes_received=self.bytes_received,
                      complete=self.completed, cancelled=self.interrupted, retries=self.retries,
                      duplicates=self.duplicates, stale=self.stale, stage=self.stage,
                      post_transfer_seconds=0 if self.recovery_since is None else self.last_tick - self.recovery_since)
        result["passed"] = self.completed and self.interrupted and result["post_transfer_seconds"] >= 1
        self.log("clipboard-stress", result)
        return result

    def close(self):
        # Best effort only: each synthetic helper lease also expires naturally.
        for slot in self.sessions:
            try:
                self.sequence += 1
                self.send(slot, 32, desktop(2, self.sequence, self.sessions[slot], ready=False))
            except OSError:
                pass
