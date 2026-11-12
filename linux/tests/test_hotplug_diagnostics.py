"""The K830 endpoint-allocation incident is separate from the live-frame stall."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

from kms_stress.diagnostics import uart_fault
from kms_stress.runner import Evidence
from kms_stress.usb_diagnostics import classify
from test_usb_campaign import config, sample


class HotplugDiagnosticsTests(unittest.TestCase):
    def test_synthetic_k830_failure_is_reported_as_allocation_without_inventing_cause(self):
        fixture = json.loads((Path(__file__).parent / 'fixtures/endpoint-allocation.json').read_text())
        before = sample(boot=fixture['boot']); before['state']['inputs'] = 4
        after = deepcopy(before); after['at'] += 1000; after['state']['inputs'] = 2
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            try:
                for line in fixture['lines']: evidence.log('uart', {'port': 'master', 'line': line})
                result = classify(before, after, [], {'errors': ['Source 2 missing'], 'observed_faults': evidence.fault_snapshot()})
                self.assertEqual(result['signature'], 'host-endpoint-allocation-failed')
                self.assertEqual(result['confidence'], 'observed-error')
                self.assertIn('unconfirmed', result['cause'])
                self.assertEqual(len(evidence.faults), 1)
                self.assertGreater(evidence.faults[0]['repeats'], 10)
                self.assertTrue(any('c52b' in line for line in fixture['lines']))
                self.assertTrue(any('standby' in line for line in fixture['lines']))
            finally: evidence.file.close()

    def test_each_sdk_error_spelling_is_recognized_and_unrelated_memory_or_channels_are_not(self):
        for message in ('USBH: EP Alloc error: ESP_ERR_NO_MEM', 'USB HOST: EP allocation error ESP_ERR_NO_MEM',
                        'USB HOST: Claiming interface error: ESP_ERR_NO_MEM'):
            self.assertEqual(uart_fault('E (123) ' + message), 'USB endpoint allocation failed')
        for message in ('heap: ESP_ERR_NO_MEM', 'USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED',
                        'HCD DWC: No more HCD channels available', 'usb-input: Address 5 interface 0: standby'):
            self.assertIsNone(uart_fault('E (123) ' + message))

    def test_repeated_allocation_logs_cannot_fill_fault_window_and_hide_later_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            try:
                for _ in range(1000): evidence.log('uart', {'port': 'master', 'line': 'E (1) USBH: EP Alloc error: ESP_ERR_NO_MEM'})
                saved = evidence.fault_snapshot()
                evidence.log('uart', {'port': 'master', 'line': 'E (2) USB HOST: EP allocation error ESP_ERR_NO_MEM'})
                evidence.log('uart', {'port': 'master', 'line': 'W (3) diagnostics: Slot 3: USB control deadline (detail=0, idle=0ms)'})
                self.assertEqual(len(evidence.faults), 2)
                self.assertEqual(evidence.faults[0]['repeats'], 1001)
                self.assertEqual(saved[0]['repeats'], 1000)  # Frozen evidence cannot mutate with later logs.
                self.assertEqual(evidence.faults[1]['event'], 'USB control deadline')
            finally: evidence.file.close()

    def test_receiver_uart_error_does_not_masquerade_as_master_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            try:
                evidence.log('uart', {'port': 'other', 'line': 'E (1) USBH: EP Alloc error: ESP_ERR_NO_MEM'})
                self.assertEqual(evidence.fault_snapshot(), [])
            finally: evidence.file.close()

    def test_missing_status_or_board_reset_is_inconclusive(self):
        before, after = sample(), sample()
        item = {'errors': [], 'observed_faults': [{'event': 'USB endpoint allocation failed'}]}
        self.assertEqual(classify(before, None, [], item)['confidence'], 'incomplete')
        after['boot'] += 1
        self.assertEqual(classify(before, after, [], item)['signature'], 'master-reset')

    def test_previous_attempt_errors_are_not_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Evidence(directory, config(directory))
            try:
                evidence.log('uart', {'port': 'master', 'line': 'E (1) USBH: EP Alloc error: ESP_ERR_NO_MEM'})
                first = evidence.fault_snapshot()
                with evidence.lock: evidence.faults.clear()  # Same boundary used by Runner.segment.
                self.assertEqual(classify(sample(), sample(), [], {'errors': [], 'observed_faults': evidence.fault_snapshot()})['signature'], 'no-failure')
                self.assertEqual(first[0]['event'], 'USB endpoint allocation failed')
            finally: evidence.file.close()


if __name__ == '__main__': unittest.main()
