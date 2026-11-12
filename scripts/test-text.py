"""Exercise production batch transport, USB completion, retries and cancellation."""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]

compiler = NativeCompiler()
out = root / 'build/text-tests'
out.mkdir(parents=True, exist_ok=True)
objects = []
sources = ['tests/native/test_text.c', 'tests/native/runtime.c'] + [
    'firmware/components/kms_core/' + name for name in ('text.c', 'log_buffer.c', 'wire.c')]
for source in sources:
    obj = out / (Path(source).stem + '.obj')
    subprocess.run(compiler.compile_command(
        root / source, obj,
        includes=[
            root / 'tests/native/include',
            root / 'firmware/components/kms_core/include',
            root / 'config'],
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'text-tests.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_text_tests()
if failure:
    line = (root / 'tests/native/test_text.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_text.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_text_checks()} batch transport, partial tail, completion, retry and cancellation checks')
