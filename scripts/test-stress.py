"""Run independent Linux-app tests and native generator/trace tests on Linux or Windows."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
out = root / 'build/stress-tests'
out.mkdir(parents=True, exist_ok=True)

compiler = NativeCompiler()
sources = [root / p for p in ('diagnostics/hid-generator/main/pattern.c',
    'firmware/components/kms_core/hid_parser.c', 'firmware/components/kms_core/management.c',
    'tests/native/runtime.c', 'tests/native/test_stress.c',
    'diagnostics/usb-isolation/main/stream.c', 'tests/native/test_usb_isolation_stream.c',
    'diagnostics/usb-isolation/main/trace_store.c', 'tests/native/test_usb_isolation_trace.c')]
objects = []
for source in sources:
    obj = out / (source.stem + '.obj')
    includes = ['tests/native/stress/include', 'tests/native/include',
                'firmware/components/kms_core/include', 'firmware/components/kms_runtime/include',
                'diagnostics/hid-generator/main', 'diagnostics/usb-isolation/main', 'config']
    subprocess.run(compiler.compile_command(
        source, obj,
        includes=[root / p for p in includes],
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
generator_source = os.environ.get('KMS_GENERATOR_SOURCE')
generator_object = out / 'test_generator_lifecycle.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_generator_lifecycle.c', generator_object,
    includes=[root / p for p in ('tests/native/generator/include', 'tests/native/stress/include', 'tests/native/include', 'firmware/components/kms_core/include', 'firmware/components/kms_runtime/include', 'diagnostics/hid-generator/main', 'config')],
    defines=[
        'KMS_GENERATOR_TRACE=1',
        *(['KMS_GENERATOR_SOURCE="' + Path(generator_source).as_posix() + '"'] if generator_source else [])],
    disabled_warnings=[4244, 4267]), check=True)
objects.append(str(generator_object))
isolation_object = out / 'test_usb_isolation_host.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_usb_isolation_host.c', isolation_object,
    includes=[root / p for p in ('tests/native/isolation/include', 'tests/native/generator/include', 'tests/native/usb-host/include', 'tests/native/stress/include', 'tests/native/include', 'diagnostics/usb-isolation/main', 'diagnostics/hid-generator/main')],
    disabled_warnings=[4244, 4267]), check=True)
objects.append(str(isolation_object))
capture_object = out / 'test_usb_hcd_capture.obj'
capture_source = os.environ.get('KMS_HCD_CAPTURE_SOURCE')
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_usb_hcd_capture.c', capture_object,
    includes=[root / p for p in ('tests/native/isolation-trace/include', 'tests/native/usb-host/include', 'tests/native/include', 'diagnostics/usb-isolation/main')],
    defines=['KMS_HCD_CAPTURE_SOURCE="' + Path(capture_source).as_posix() + '"'] if capture_source else [],
    disabled_warnings=[4244, 4267]), check=True)
objects.append(str(capture_object))

generator_trace_object = out / 'test_generator_trace.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_generator_trace.c', generator_trace_object,
    includes=[root / p for p in ('tests/native/generator/include', 'tests/native/stress/include', 'tests/native/usb-host/include', 'tests/native/include', 'diagnostics/hid-generator/main')],
    disabled_warnings=[4244, 4267]), check=True)
objects.append(str(generator_trace_object))

guard_object = out / 'test_usb_frame_guard.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_usb_frame_guard.c', guard_object,
    includes=[root / p for p in ('tests/native/stress/include', 'tests/native/usb-host/include', 'tests/native/include', 'diagnostics/usb-isolation/main')],
    disabled_warnings=[4244, 4267]), check=True)
objects.append(str(guard_object))

dll = out / 'kms-stress-tests.dll'
subprocess.run(compiler.link_command(
    dll, objects,
    exports=[name for name in ('kms_generator_pattern', 'kms_test_init', 'kms_test_record', 'kms_test_command', 'kms_test_page', 'kms_test_mode_set', 'iso_stream_report')]), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_stress_trace_tests()
if failure: raise SystemExit(f'FAIL native trace check at test_stress.c:{failure}')
print(f'PASS: {tests.kms_stress_trace_test_count()} native trace-ring/CRC/injection checks')
failure = tests.kms_run_generator_lifecycle_tests()
if failure: raise SystemExit(f'FAIL generator lifecycle check at test_generator_lifecycle.c:{failure}')
print(f'PASS: {tests.kms_generator_lifecycle_check_count()} production generator lifecycle checks')
failure = tests.kms_run_isolation_stream_tests()
if failure: raise SystemExit(f'FAIL isolation stream check at test_usb_isolation_stream.c:{failure}')
print(f'PASS: {tests.kms_isolation_stream_check_count()} production isolation stream/descriptor checks')
failure = tests.kms_run_isolation_host_tests()
if failure: raise SystemExit(f'FAIL isolation host check at test_usb_isolation_host.c:{failure}')
print(f'PASS: {tests.kms_isolation_host_check_count()} production isolation ownership/watchdog/freeze checks')
failure = tests.kms_run_driver_trace_tests()
if failure: raise SystemExit(f'FAIL driver trace check at test_usb_isolation_trace.c:{failure}')
print(f'PASS: {tests.kms_driver_trace_check_count()} driver trace storage/freeze checks')

failure = tests.kms_run_hcd_capture_tests()
if failure: raise SystemExit(f'FAIL HCD capture check at test_usb_hcd_capture.c:{failure}')
print(f'PASS: {tests.kms_hcd_capture_check_count()} actual HCD observer/scheduler/bounds checks')

failure = tests.kms_run_generator_trace_tests()
if failure: raise SystemExit(f'FAIL generator trace check at test_generator_trace.c:{failure}')
print(f'PASS: {tests.kms_generator_trace_check_count()} production generator register/ring/freeze checks')

failure = tests.kms_run_frame_guard_tests()
if failure: raise SystemExit(f'FAIL frame guard check at test_usb_frame_guard.c:{failure}')
print(f'PASS: {tests.kms_frame_guard_check_count()} frame guard checks across {tests.kms_frame_guard_scenario_count()} phase/mode/endpoint scenarios')

env = {**os.environ, 'PYTHONPATH': str(root / 'linux'), 'KMS_STRESS_PATTERN_DLL': str(dll)}
subprocess.run([sys.executable, str(root / 'scripts/test-channel-recovery.py'), '--stress'], check=True)
subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', str(root / 'linux/tests'), '-v'], env=env, check=True)
