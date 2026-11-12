"""Compile and exercise the production desktop protocol/routing core with the native host compiler."""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
out = root / 'build/desktop-tests'
out.mkdir(parents=True, exist_ok=True)

compiler = NativeCompiler()
core = root / 'firmware/components/kms_core'
objects = []
for source in [root / 'firmware/components/kms_runtime/stream_runtime.c', core / 'clipboard.c', core / 'desktop.c', core / 'management.c', core / 'cached_update.c', core / 'stream.c', core / 'wire.c', root / 'tests/native/test_desktop.c',
               root / 'tests/native/test_desktop_runtime.c', root / 'tests/native/test_clipboard.c', root / 'tests/native/routing_probe.c', root / 'tests/native/runtime.c']:
    obj = out / (source.stem + '.obj')
    subprocess.run(compiler.compile_command(
        source, obj,
        includes=[
            core / 'include',
            root / 'tests/native/stream/include',
            root / 'config',
            root / 'tests/native/desktop/include',
            root / 'tests/native/include',
            root / 'firmware/components/kms_runtime/include'],
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'desktop-tests.dll'
subprocess.run(compiler.link_command(dll, objects, stack_probes=True), check=True)
library = ctypes.CDLL(str(dll))
failure = library.kms_run_desktop_tests()
if failure:
    line = (root / 'tests/native/test_desktop.c').read_text().splitlines()[failure - 1]
    values = [library.kms_desktop_test_debug(i) for i in range(5)]
    raise SystemExit(f'FAIL test_desktop.c:{failure}: {line.strip()} (target, x, y, selected, result={values})')
print(f'PASS: {library.kms_desktop_test_count()} production desktop codec, geometry, routing, lease and session checks')
failure = library.kms_run_desktop_runtime_tests()
if failure:
    line = (root / 'tests/native/test_desktop_runtime.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_desktop_runtime.c:{failure}: {line.strip()}')
print(f'PASS: {library.kms_desktop_runtime_test_count()} production S2 USB mailbox/radio bridge checks')
failure = library.kms_run_clipboard_tests()
if failure:
    raise SystemExit(f'FAIL test_clipboard.c:{failure}')
print(f'PASS: {library.kms_clipboard_test_count()} clipboard codec, corruption and session authorization checks')
