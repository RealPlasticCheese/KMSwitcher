"""Retain the diagnostic overlay API with the shared production guard hook."""
import importlib.util
from pathlib import Path
import sys
path = Path(__file__).resolve().parents[2] / "firmware/usb_frame_guard/instrument_hal.py"
spec = importlib.util.spec_from_file_location("kms_guard_hal", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SHA256 = module.SHA256
def instrument(data):
    return module.instrument(data, trace=True)
if __name__ == "__main__":Path(sys.argv[2]).write_text(instrument(Path(sys.argv[1]).read_bytes()))
