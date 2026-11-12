"""Test production console block output with a fake UART, VFS and shared stdio."""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]

compiler = NativeCompiler()
out = root / 'build/console-output-tests'
out.mkdir(parents=True, exist_ok=True)
source = root / 'tests/native/test_console_output.c'
obj = out / 'console-output.obj'
subprocess.run(compiler.compile_command(
    source, obj,
    includes=[
        root / 'tests/native/console-output/include',
        root / 'tests/native/include',
        root / 'firmware/controller/main']), check=True)
dll = out / 'console-output-tests.dll'
subprocess.run(compiler.link_command(dll, [str(obj)]), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_console_output_tests()
if failure:
    line = source.read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL {source.name}:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_console_output_test_count()} block output, CRLF, binary, locking, UART failure and initialization checks')
