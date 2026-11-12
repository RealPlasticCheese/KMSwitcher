"""Run production stream codec, pool, negotiation, scheduling and rate policy."""
import ctypes
from pathlib import Path
import subprocess
from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
out = root / 'build/stream-tests'
out.mkdir(parents=True, exist_ok=True)
remote = (root / 'firmware/components/kms_runtime/remote.c').read_text(encoding='utf-8')
(out / 'remote_stream.inc').write_text(remote[remote.index('typedef struct { uint32_t id, target, offset, token, at;'):
    remote.index('static void accept_summary(')], encoding='utf-8')
compiler = NativeCompiler()
core = root / 'firmware/components/kms_core'
runtime = root / 'firmware/components/kms_runtime'
objects = []
for source in [core / 'stream.c', core / 'management.c', core / 'wire.c', runtime / 'stream_runtime.c', runtime / 'stream_radio.c',
               root / 'tests/native/test_stream.c', root / 'tests/native/test_remote_stream.c', root / 'tests/native/runtime.c']:
    obj = out / (source.stem + '.obj')
    subprocess.run(compiler.compile_command(source, obj, includes=[root / 'tests/native/stream/include',
        root / 'tests/native/desktop/include', root / 'tests/native/include', core / 'include',
        runtime / 'include', root / 'config', out], disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'stream-tests.dll'
subprocess.run(compiler.link_command(dll, objects, stack_probes=True), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_stream_tests()
if failure:
    raise SystemExit(f'FAIL test_stream.c:{failure}')
print(f'PASS: {tests.kms_stream_checks()} stream integrity, bounds, pool, mixed-version, input priority and radio fallback checks')

failure = tests.kms_run_remote_stream_tests()
if failure:
    raise SystemExit(f'FAIL test_remote_stream.c:{failure}')
print('PASS: asynchronous OTA forwarding, pending bounds, matched receipts and congestion checks')
