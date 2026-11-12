import struct
import unittest
from unittest.mock import Mock
from kms_update import management as m
from test_management import Clock, Serial, line, reply


class RadioDiagnosticsTests(unittest.TestCase):
    def test_decoder_exposes_completion_failure_and_timing_fields(self):
        status = m.decode_radio(struct.pack('<20I', 1, 22, *range(2, 20)))
        self.assertEqual((status['boot'], status['tx_failed'], status['max_rx_gap_ms'], status['min_heap']),
                         ('00000016', 5, 13, 19))
        for data in (b'', bytes(80), bytes(84)):
            with self.assertRaises(ValueError): m.decode_radio(data)

    def test_real_wire_request_is_capability_and_boot_bound(self):
        clock = Clock()
        serial = Serial(lambda command, count: [line(reply(command, struct.pack('<20I', 1, 22, *range(2, 20))))], clock)
        controller = m.Controller(serial, timeout=.1, clock=clock); controller.boots[2] = 22
        identity = dict(capabilities=m.CAP_RADIO_DIAGNOSTICS, boot='00000016')
        self.assertEqual(controller.radio_status(2, identity)['tx_failed'], 5)
        sent = m.Frame.decode(bytes.fromhex(serial.sent[0].decode().split()[2]))
        self.assertEqual((sent.op, sent.target, sent.data), (21, 22, b''))
        identity['boot'] = '00000017'
        with self.assertRaisesRegex(ValueError, 'boot'): controller.radio_status(2, identity)

    def test_old_receiver_does_not_receive_an_unknown_opcode(self):
        controller = m.Controller(Mock())
        self.assertIsNone(controller.radio_status(2, dict(capabilities=7, boot='00000016')))
        controller.serial.write.assert_not_called()
