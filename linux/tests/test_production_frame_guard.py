"""Pin the exact HAL hook while retaining the standalone diagnostic source API."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


class ProductionGuardTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2]
        path = self.root / 'firmware/usb_frame_guard/instrument_hal.py'
        if not path.exists(): self.skipTest('Source overlay is outside installed runner kit')
        spec = importlib.util.spec_from_file_location('test_guard_hal', path)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.data = ('#include <stddef.h>\n' + self.module.ANCHOR).encode()

    def test_changed_sdk_or_already_guarded_source_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unreviewed'): self.module.instrument(self.data)

    def test_exact_chena_hook_is_shared_but_trace_is_optional(self):
        # Fake the hash only for this compact source fixture. The actual SDK hash
        # and generated object are tested by scripts/test-usb-stack.py and builds.
        with patch.object(self.module.hashlib, 'sha256') as digest:
            digest.return_value.hexdigest.return_value = self.module.SHA256
            production = self.module.instrument(self.data)
            diagnostic = self.module.instrument(self.data, trace=True)
        for content, prefix in [(production, 'kms_usb_frame_guard'), (diagnostic, 'iso_guard')]:
            before = content.index(prefix + '_before_enable(')
            enable = content.index('usb_dwc_ll_hcchar_enable_chan(')
            after = content.index(prefix + '_after_enable(')
            self.assertLess(before, enable); self.assertLess(enable, after)
            self.assertIn('chan_obj->type == USB_DWC_XFER_TYPE_INTR && chan_obj->regs->hcchar_reg.epdir', content)
            self.assertEqual(content.count('usb_dwc_ll_hcchar_enable_chan('), 1)
        self.assertNotIn('driver_trace.h', production)
        self.assertNotIn('iso_driver_', production)
        self.assertIn('iso_driver_enable_edge(', diagnostic)

    def test_ambiguous_or_missing_sdk_anchor_is_rejected_even_after_hash_review(self):
        with patch.object(self.module.hashlib, 'sha256') as digest:
            digest.return_value.hexdigest.return_value = self.module.SHA256
            for data in [b'other function', self.data + self.data]:
                with self.assertRaisesRegex(ValueError, 'Ambiguous'): self.module.instrument(data)

    def test_diagnostic_entry_point_uses_shared_generator(self):
        path = self.root / 'diagnostics/usb-isolation/instrument_hal.py'
        spec = importlib.util.spec_from_file_location('test_diag_guard', path)
        diagnostic = importlib.util.module_from_spec(spec); spec.loader.exec_module(diagnostic)
        self.assertEqual(diagnostic.SHA256, self.module.SHA256)
        with self.assertRaisesRegex(ValueError, 'Unreviewed'): diagnostic.instrument(b'changed SDK')
