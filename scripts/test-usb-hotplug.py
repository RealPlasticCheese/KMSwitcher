"""Run the complete offline hot-plug matrix, SDK allocators/HAL and stress regressions.

Requires the native host C compiler and installed pinned ESP SDK.
Does not open serial ports, flash boards or interrupt the working input path.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
for name in ('test-usb-host.py', 'test-usb-shared-heap.py', 'test-usb-stack.py', 'test-stress.py'):
    print('Running ' + name, flush=True)
    subprocess.run([sys.executable, str(root / 'scripts' / name)], check=True, timeout=60, cwd=root)
print('PASS: complete offline USB hot-plug matrix and stress regression suites')
