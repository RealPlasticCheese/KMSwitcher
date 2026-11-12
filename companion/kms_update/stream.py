"""Firmware endpoint adapter for the shared stream pump."""
import re
import struct
from kms_transport import PROFILES, Receipt, StreamLimits, send_stream


class FirmwareDestination:
    def __init__(self, controller, slot, operation, token):
        self.controller, self.slot, self.operation, self.token = controller, slot, operation, token
        self.requests = {}
        self.ready = []
        self.pipelined = False

    def open(self, size, profile):
        from .management import Controller
        if isinstance(self.controller, Controller):
            self.pipelined = self.controller.configure_stream(self.slot, profile)
        return 0, StreamLimits(188, window=4 if self.pipelined else 1, stall=45)

    def submit(self, offset, data):
        from .management import Frame, ManagementError, OFFSET
        c = self.controller
        payload = struct.pack('<I', self.token) + data
        if not self.pipelined:
            response = c.request(self.slot, self.operation, payload, offset=offset)
            if response.data != struct.pack('<I', offset + len(data)):
                raise ManagementError(OFFSET)
            self.ready.append(Receipt(offset, offset + len(data)))
            self.requests[offset] = None
            return offset
        c.sequence = (c.sequence + 1) & 0xffffffff or 1
        q = Frame(self.operation, self.slot or 1, c.sequence, target=c.boots[self.slot], offset=offset, data=payload)
        self.requests[q.request_id] = q
        self.retry(q.request_id)
        return q.request_id

    def retry(self, key):
        if not self.pipelined:
            raise RuntimeError('Synchronous firmware receipt was lost')
        q = self.requests[key]
        self.controller.command(f'master {q.encode().hex()}' if not self.slot else f'remote {self.slot} {q.encode().hex()}')

    def poll(self, timeout):
        from .management import Frame, ManagementError, OK, BUSY, OFFSET, TIMEOUT, REPLY, SUMMARY, WRONG_BOOT
        if self.ready:
            return self.ready.pop(0)
        c = self.controller
        for line in c.lines_until(c.clock() + timeout):
            match = re.fullmatch(r'KMSMASTER ([0-9a-fA-F]+)' if not self.slot else r'KMSM (\d+) ([0-9a-fA-F]+)', line)
            if not match or (self.slot and int(match[1]) != self.slot):
                continue
            try:
                r = Frame.decode(bytes.fromhex(match[2 if self.slot else 1]))
            except ValueError:
                continue
            q = self.requests.get(r.request_id)
            if (q is None or r.op != q.op or r.slot != q.slot or r.offset != q.offset or
                    not r.flags & REPLY or r.flags & SUMMARY or
                    (r.sender != q.target and r.status != WRONG_BOOT)):
                continue
            if r.status in (BUSY, OFFSET, TIMEOUT):
                return None  # A gap or queue pressure is retried with the same identity.
            if r.status != OK:
                raise ManagementError(r.status)
            end = q.offset + len(q.data) - 4
            if r.data != struct.pack('<I', end):
                raise ManagementError(OFFSET)
            return Receipt(r.request_id, end)
        return None

    def release(self, key):
        self.requests.pop(key, None)

    def finish(self, size):
        pass  # Caller retains END/verify/activation and rollback policy.

    def close(self):
        self.requests.clear()


def upload(controller, slot, operation, token, source, size, progress):
    send_stream(source, size, FirmwareDestination(controller, slot, operation, token),
                profile=getattr(controller, 'speed_profile', PROFILES[0]), progress=progress,
                clock=controller.clock if hasattr(controller, 'clock') else __import__('time').monotonic)
