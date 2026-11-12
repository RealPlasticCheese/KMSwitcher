"""Exercise the real HID adapter/pump with USB control-transfer latency."""
from collections import Counter, deque
from dataclasses import replace
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from kms_companion.clipboard_protocol import Kind, Packet
from kms_companion.clipboard_service import TransferService
from kms_companion.protocol import Message, Op
from kms_companion.transport import DeviceError, Receiver, STREAM_STATUS_TAG


class HidDevice:
    def __init__(self, tagged):
        self.now = 10
        self.tagged = tagged
        self.incoming = deque()
        self.sent = []
        self.reads = Counter()
        self.capacity = 4

    def get_feature_report(self, report, length):
        self.reads[report] += 1
        self.now += .006 if report == 35 else .004
        if report == 33:
            data = bytearray(48); data[0] = 3; data[3] = data[44] = 1
            identifier = (STREAM_STATUS_TAG | len(self.incoming) << 16 | self.capacity << 8 | 2) if self.tagged else 0
            return bytes([33]) + Message(Op.STATUS, identifier, 101, bytes(data)).encode()
        if report == 35:
            return bytes([35]) + (self.incoming.popleft().encode() if self.incoming else bytes(192))
        if report == 36:
            data = bytearray(32); data[:5] = b'KMS2\x01'; data[5] = self.capacity; data[7] = 2; data[8] = 6
            struct.pack_into('<I', data, 12, zlib.crc32(data[:12]))
            return bytes([36]) + data
        raise AssertionError(report)

    def send_feature_report(self, wire):
        self.now += .006
        if wire[0] == 34:
            self.sent.append(Packet.decode(wire[1:]))
        elif wire[0] != 36:
            raise AssertionError(wire[0])


def receiver(device):
    value = Receiver.__new__(Receiver)
    value.device, value.session = device, 101
    value.stream_profile, value.stream_supported, value.stream_state = 2, True, {}
    return value


class UsbPumpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.service = TransferService(Path(self.temp.name))
        self.addCleanup(self.service.close)
        self.service.slot, self.service.session = 3, 101
        self.service.settings['speed'] = 'fast'
        self.service.next_scan = float('inf')
        self.base = Packet(Kind.DATA, 3, 2, 101, 202, 1, 123, data=b'x' * 160)

    def test_tagged_status_skips_empty_receive_and_extra_capacity_reads(self):
        device = HidDevice(True); link = receiver(device)
        for i in range(4): self.service._post(replace(self.base, sequence=i + 1, offset=i * 160))
        with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now):
            status = link.status(); self.service.pump(link, status, True)
        self.assertEqual(device.reads, {33: 1})
        self.assertGreaterEqual(len(device.sent), 2)
        self.assertLessEqual(len(device.sent), 4)
        self.assertEqual([p.offset for p in device.sent], [i * 160 for i in range(len(device.sent))])
        self.assertLess(device.now - 10, .030)

    def test_negotiated_firmware_without_status_extension_still_uses_a_bounded_burst(self):
        device = HidDevice(False); link = receiver(device)
        for i in range(4): self.service._post(replace(self.base, sequence=i + 1, offset=i * 160))
        with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now):
            self.service.pump(link, link.status(), True)
        self.assertEqual(device.reads[36], 1)
        self.assertGreaterEqual(len(device.sent), 2)
        self.assertLessEqual(len(device.sent), 4)

    def test_receive_drains_multiple_ready_cells_with_slow_usb_calls(self):
        device = HidDevice(True); link = receiver(device)
        device.incoming.extend(replace(self.base, sequence=i + 1) for i in range(4))
        with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now), \
             patch.object(self.service, 'deliver') as deliver:
            self.service.pump(link, link.status(), True)
        self.assertGreaterEqual(deliver.call_count, 2)
        self.assertLessEqual(deliver.call_count, 4)
        self.assertEqual(device.reads[35], deliver.call_count)
        self.assertEqual(device.reads[36], 0)

    def test_full_firmware_pool_blocks_data_but_keeps_cancel_available(self):
        device = HidDevice(True); device.capacity = 0; link = receiver(device)
        packet = replace(self.base, kind=Kind.CANCEL, data=b'')
        self.service._post(packet)
        with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now):
            self.service.pump(link, link.status(), True)
        self.assertEqual(device.sent, [packet])
        self.service._post(self.base)
        with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now):
            self.service.pump(link, link.status(), True)
        self.assertEqual(device.sent, [packet])

    def test_status_extension_bounds_and_legacy_fallback(self):
        device = HidDevice(False); link = receiver(device)
        self.assertNotIn('stream', link.status())
        device.tagged = True; device.capacity = 5
        with self.assertRaises(DeviceError): link.status()

    def test_status_profile_change_is_written_once_without_capacity_poll(self):
        device = HidDevice(True); link = receiver(device); link.stream_profile = 0
        status = link.status()
        self.assertEqual(link.stream_capacity('fast', status['stream']), 4)
        self.assertEqual(link.stream_capacity('fast', status['stream']), 4)
        self.assertEqual(device.reads, {33: 1})
        self.assertEqual(link.stream_profile, 2)

    def test_latency_model_removes_the_one_packet_per_poll_ceiling(self):
        def submission_rate(tagged, budget):
            device = HidDevice(tagged); link = receiver(device)
            while not self.service.radio_out.empty(): self.service.radio_out.get_nowait()
            for i in range(4): self.service._post(replace(self.base, sequence=i + 1, offset=i * 160))
            with patch('kms_companion.clipboard_service.time.monotonic', side_effect=lambda: device.now), \
                 patch('kms_companion.clipboard_service.RADIO_CREDIT_BUDGET', budget):
                self.service.pump(link, link.status(), True)
            device.now += .005  # Existing owner-loop idle interval.
            return len(device.sent) * 160 / (device.now - 10)

        previous = submission_rate(False, .002)
        updated = submission_rate(True, .012)
        self.assertGreater(updated, previous * 2)
        print(f'USB submission model: {previous / 1024:.2f} -> {updated / 1024:.2f} KiB/s; '
              f'{updated / previous:.2f}x (6 ms feature transfers; excludes radio/receipts)')
