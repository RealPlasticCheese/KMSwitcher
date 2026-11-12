"""Run production Keyboard Console entry, typing, editing and exit callbacks."""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler
root = Path(__file__).resolve().parents[1]

compiler = NativeCompiler()
out = root / 'build/keyboard-console-tests'
out.mkdir(parents=True, exist_ok=True)
includes = [root / p for p in ('tests/native/keyboard-console/include', 'tests/native/history-store/include', 'tests/native/include', 'firmware/components/kms_core/include', 'firmware/components/kms_runtime/include', 'config')]
objects = []
for source in ('tests/native/test_keyboard_console.c', 'tests/native/runtime.c', 'firmware/components/kms_core/router.c', 'firmware/components/kms_core/key_trace.c', 'firmware/components/kms_core/log_buffer.c', 'firmware/components/kms_core/wire.c', 'firmware/components/kms_core/user_preferences.c', 'firmware/components/kms_core/management.c'):
    obj = out / (Path(source).stem + '.obj')
    subprocess.run(compiler.compile_command(
        root / source, obj,
        includes=[p for p in includes],
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'keyboard-console.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_keyboard_console_tests()
if failure:
    line = (root/'tests/native/test_keyboard_console.c').read_text().splitlines()[failure-1]
    raise SystemExit(f'FAIL test_keyboard_console.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_keyboard_console_checks()} production console entry, idle, typing, editing, worker response and Escape checks')
