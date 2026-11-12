"""Run the production S3 recovery policy against a deterministic radio and clock."""
import ctypes
from pathlib import Path
import subprocess
import sys

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
stress_only = sys.argv[1:] == ["--stress"]
out = root / "build" / ("stress-tests/channel-recovery" if stress_only else "channel-recovery-tests")
out.mkdir(parents=True, exist_ok=True)
compiler = NativeCompiler()
test = root / "tests/native/test_channel_recovery.c"
includes = [root / path for path in (
    "tests/native/radio/include", "tests/native/usb-host/include", "tests/native/include",
    "firmware/components/kms_core/include", "firmware/components/kms_runtime/include", "config",
)]
objects = []
for source in (test, root / "tests/native/runtime.c"):
    obj = out / (source.stem + ".obj")
    subprocess.run(compiler.compile_command(source, obj, includes=includes,
                                            disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / "kms-channel-recovery.dll"
subprocess.run(compiler.link_command(dll, objects), check=True)
suite = ctypes.CDLL(str(dll))
kind = "stress" if stress_only else "unit"
failure = getattr(suite, f"kms_run_channel_recovery_{kind}")()
if failure:
    line = test.read_text().splitlines()[failure - 1]
    raise SystemExit(f"FAIL {test.name}:{failure}: {line.strip()}")
print(f"PASS: {suite.kms_channel_recovery_checks()} channel recovery {kind} checks")
