"""Shared sender against bounded streams and a lossy firmware endpoint."""
from collections import OrderedDict, deque
import io
import struct
import unittest
from kms_transport import Receipt, StreamLimits, send_stream
from kms_update.management import Controller, Frame, WRITE, STREAM_STATUS, REPLY, OK, OFFSET
from kms_update.stream import FirmwareDestination


class Destination:
    def __init__(self):
        self.pending = OrderedDict()
        self.committed = self.now = 0
        self.maximum = self.retries = 0
        self.drop = False
        self.closed = self.finished = False

    def open(self, size, profile):
        return 0, StreamLimits(188, 4)

    def submit(self, offset, data):
        self.pending[offset] = data
        self.maximum = max(self.maximum, len(self.pending))
        return offset

    def poll(self, timeout):
        self.now += timeout
        if not self.pending or self.drop:
            return None
        offset = next(iter(self.pending))
        self.committed = offset + len(self.pending[offset])
        return Receipt(offset, self.committed)

    def retry(self, key):
        self.retries += 1

    def release(self, key):
        del self.pending[key]

    def finish(self, size):
        self.finished = True

    def close(self):
        self.closed = True


class SerialFirmware:
    def __init__(self):
        self.now = 100
        self.lines = deque()
        self.image = bytearray()
        self.cache = OrderedDict()
        self.dropped = set()
        self.writes = []

    def write(self, command):
        parts = command.decode().split()
        q = Frame.decode(bytes.fromhex(parts[-1]))
        r = Frame(q.op, q.slot, q.request_id, sender=q.target, offset=q.offset, flags=REPLY)
        if q.op == STREAM_STATUS:
            r.data = bytes(32)
        else:
            self.writes.append((q.request_id, q.offset, q.data))
            if q.offset == 188 and q.offset not in self.dropped:
                self.dropped.add(q.offset)
                return
            if q.request_id in self.cache:
                r = self.cache[q.request_id]
            elif q.offset != len(self.image):
                r.status = OFFSET
            else:
                self.image.extend(q.data[4:])
                r.data = struct.pack('<I', len(self.image))
                self.cache[q.request_id] = r
                if len(self.cache) > 4:
                    self.cache.popitem(last=False)
        self.lines.append((f'KMSM {q.slot} ' if parts[0] == 'remote' else 'KMSMASTER ').encode() + r.encode().hex().encode() + b'\n')

    def flush(self):
        pass

    def readline(self, size):
        if self.lines:
            return self.lines.popleft()
        self.now += .01
        return b''


class StreamTests(unittest.TestCase):
    def test_source_read_ahead_is_bounded_and_only_commits_report_progress(self):
        destination = Destination()
        source = io.BytesIO(bytes(4097))
        positions = []
        def check():
            positions.append(source.tell() - destination.committed)
        progress = []
        send_stream(source, 4097, destination, check=check, clock=lambda: destination.now,
                    progress=lambda done, total: progress.append(done))
        self.assertLessEqual(max(positions), 4 * 188)
        self.assertEqual(destination.maximum, 4)
        self.assertEqual(progress[-1], 4097)
        self.assertTrue(destination.finished and destination.closed)

    def test_cancellation_releases_all_slots_during_retry_backoff(self):
        destination = Destination(); destination.drop = True
        def check():
            if destination.now >= 1:
                raise InterruptedError('cancelled')
        with self.assertRaises(InterruptedError):
            send_stream(io.BytesIO(bytes(1000)), 1000, destination, check=check, clock=lambda: destination.now)
        self.assertFalse(destination.pending)
        self.assertTrue(destination.closed)
        self.assertFalse(destination.finished)
        self.assertEqual(destination.retries, 4)

    def test_a_stall_has_a_deadline_and_bounded_retry_count(self):
        destination = Destination(); destination.drop = True
        with self.assertRaises(TimeoutError):
            send_stream(io.BytesIO(bytes(1000)), 1000, destination, clock=lambda: destination.now)
        self.assertLess(destination.now, 30.1)
        self.assertLess(destination.retries, 80)
        self.assertFalse(destination.pending)

    def test_impossible_receipt_cannot_advance_progress(self):
        destination = Destination()
        destination.poll = lambda timeout: Receipt(0, 5)
        with self.assertRaisesRegex(ValueError, 'unexpected offset'):
            send_stream(io.BytesIO(bytes(1000)), 1000, destination)
        self.assertTrue(destination.closed)

    def test_firmware_pipeline_recovers_a_gap_and_preserves_request_identity(self):
        serial = SerialFirmware()
        controller = Controller(serial, clock=lambda: serial.now, speed_profile='fast')
        controller.boots = {0: 42, 1: 99}; controller.capabilities = {0: 64, 1: 64}
        image = bytes(range(251)) * 17
        destination = FirmwareDestination(controller, 1, WRITE, 123)
        send_stream(io.BytesIO(image), len(image), destination, profile='fast', clock=controller.clock)
        self.assertEqual(serial.image, image)
        attempts = [row for row in serial.writes if row[1] == 188]
        self.assertGreater(len(attempts), 1)
        self.assertEqual(len(set(attempts)), 1)
        self.assertFalse(destination.requests)

    def test_invalid_buffer_budgets_are_rejected(self):
        for block, window in ((0, 4), (188, 0), (65537, 1), (188, 17)):
            with self.assertRaises(ValueError): StreamLimits(block, window)
