"""Exercise USB discovery/selection and cancellation using a fake asynchronous host."""
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from native_build import NativeCompiler

if len(sys.argv) == 3 and sys.argv[1] == '--run-hotplug':
    matrix = ctypes.CDLL(sys.argv[2])
    failure = matrix.kms_run_usb_hotplug_tests()
    if failure:
        fixture = Path(__file__).resolve().parents[1] / 'tests/native/test_usb_hotplug_matrix.c'
        line = fixture.read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL test_usb_hotplug_matrix.c:{failure}: {line.strip()} (case={matrix.kms_usb_hotplug_case() & 0xffffffff:08x})')
    print(f'PASS: {matrix.kms_usb_hotplug_checks()} hot-plug allocation checks across {matrix.kms_usb_hotplug_cases()} matrix cases')
    root = Path(__file__).resolve().parents[1]
    Path(sys.argv[2]).with_suffix('.json').write_text(json.dumps({
        'status': 'PASS', 'cases': matrix.kms_usb_hotplug_cases(), 'checks': matrix.kms_usb_hotplug_checks(),
        'production_sha256': hashlib.sha256((root / 'firmware/controller/main/usb_devices.c').read_bytes()).hexdigest(),
        'axes': {'receiver_layout': ['c52b: keyboard/pointer/unsupported', 'c534: keyboard/pointer'],
                 'other_receiver': [False, True], 'packet_bytes': [8, 64],
                 'failure_stage': ['none', 'first claim', 'second claim', 'parser', 'control transfer', 'input transfer'],
                 'failure_duration': ['once', 'three failures', 'persistent until explicitly cleared'],
                 'ordering': ['drained then fresh address', 'fresh address during delayed cancellation', 'address reuse after drain'],
                 'clock': ['normal', 'uint32 wrap']},
        'groups': {'base': 768, 'intervention': 40, 'interrupted_probe': 216, 'both_receivers_waiting': 20, 'deferred_sdk_controls': 8},
        'limits': 'Compact synthetic descriptors and deterministic host/heap adapters; not physical DMA/RTOS/RF emulation'
    }, indent=2) + '\n')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]

compiler = NativeCompiler()
out = root / 'build/usb-host-tests'
out.mkdir(parents=True, exist_ok=True)
compiled = {}

def compile_source(source, variant='', override=None):
    key = (source, variant)
    if key in compiled:
        return compiled[key]
    obj = out / (source.stem + variant + '.obj')
    subprocess.run(compiler.compile_command(
        source, obj,
        includes=[
            *([override] if override else []),
            root / 'tests/native/usb-host/include',
            root / 'tests/native/include',
            root / 'firmware/controller/main',
            root / 'firmware/components/kms_core/include',
            root / 'firmware/components/kms_runtime/include',
            root / 'config'],
        disabled_warnings=[4244, 4267]), check=True)
    compiled[key] = str(obj)
    return str(obj)

sources = [root / 'tests/native/test_usb_host.c', root / 'tests/native/runtime.c',
    root / 'firmware/controller/main/usb_health.c',
    root / 'firmware/components/kms_core/hid_parser.c', root / 'firmware/components/kms_core/usb_policy.c']
objects = [compile_source(source) for source in sources]
dll = out / 'usb-host-tests.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_usb_host_tests()
if failure:
    line = (root / 'tests/native/test_usb_host.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_host.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_usb_host_test_count()} asynchronous USB discovery, budget, selection, cancellation and unplug checks')

matrix_obj = compile_source(root / 'tests/native/test_usb_hotplug_matrix.c')
matrix_dll = out / 'usb-hotplug-tests.dll'
subprocess.run(compiler.link_command(matrix_dll, [matrix_obj, *objects[1:]]), check=True)
subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-hotplug', str(matrix_dll)], check=True, timeout=30)

# Verify sensitivity against the discovered policy bug, without editing sources.
before_dir = out / 'before-policy-isolation'; before_dir.mkdir(exist_ok=True)
production = (root / 'firmware/controller/main/usb_devices.c').read_text()
guard = 'else if (!i->active && i->info.state != KMS_USB_UNSUPPORTED)'
if production.count(guard) != 1: raise SystemExit('Review policy-isolation regression mutation')
(before_dir / 'usb_devices.c').write_text(production.replace(guard, 'else if (!i->active)'))
before_obj = compile_source(root / 'tests/native/test_usb_hotplug_matrix.c', '-before-policy', before_dir)
before_dll = out / 'before-policy-isolation.dll'
subprocess.run(compiler.link_command(before_dll, [before_obj, *objects[1:]]), check=True)
rejected = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-hotplug', str(before_dll)],
                          capture_output=True, text=True, timeout=30)
if not rejected.returncode or 'CHECK(claim_attempts == before)' not in rejected.stdout + rejected.stderr:
    raise SystemExit('FAIL: previous policy implementation was not rejected: ' + rejected.stdout + rejected.stderr)
print('PASS: previous policy implementation rejected by receiver-isolation regression')
(out / 'policy-before-after.json').write_text(json.dumps({'current': 'PASS', 'previous': 'REJECTED',
    'previous_output': rejected.stdout + rejected.stderr}, indent=2) + '\n')

# The supervisor uses real control watches, with deterministic root-port/reset I/O.
recovery_obj = compile_source(root / 'tests/native/test_usb_recovery.c')
recovery_dll = out / 'usb-recovery-tests.dll'
subprocess.run(compiler.link_command(
    recovery_dll, [str(recovery_obj), str(out / 'runtime.obj'), str(out / 'usb_health.obj')]), check=True)
recovery_tests = ctypes.CDLL(str(recovery_dll))
failure = recovery_tests.kms_run_usb_recovery_tests()
if failure:
    line = (root / 'tests/native/test_usb_recovery.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_recovery.c:{failure}: {line.strip()}')
print(f'PASS: {recovery_tests.kms_usb_recovery_test_count()} USB watchdog, cancellation, reconnect, OTA guard and restart-limit checks')

input_sources = [root / 'tests/native/test_usb_input.c', root / 'tests/native/runtime.c',
    root / 'firmware/controller/main/usb_health.c']
input_sources += [root / 'firmware/components/kms_core' / name for name in ('hid_parser.c', 'router.c', 'wire.c', 'user_preferences.c', 'management.c')]
input_dll = out / 'usb-input-tests.dll'
input_sources += [root / 'firmware/components/kms_core/key_trace.c']
subprocess.run(compiler.link_command(input_dll, [*[compile_source(source) for source in input_sources]]), check=True)
input_tests = ctypes.CDLL(str(input_dll))
failure = input_tests.kms_run_usb_input_tests()
if failure:
    line = (root / 'tests/native/test_usb_input.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_input.c:{failure}: {line.strip()}')
print(f'PASS: {input_tests.kms_usb_input_test_count()} USB owner/app isolation, copied reports, overflow, stale input, hot-plug and recovery checks')

trace_dll = out / 'key-trace-tests.dll'
trace_sources = [root / 'tests/native/test_controller_key_trace.c', root / 'tests/native/runtime.c',
                 root / 'firmware/components/kms_core/key_trace.c', root / 'firmware/components/kms_core/wire.c']
subprocess.run(compiler.link_command(trace_dll, [*[compile_source(source) for source in trace_sources]]), check=True)
trace_tests = ctypes.CDLL(str(trace_dll))
failure = trace_tests.kms_run_controller_key_trace_tests()
if failure:
    line = (root / 'tests/native/test_controller_key_trace.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_controller_key_trace.c:{failure}: {line.strip()}')
print(f'PASS: {trace_tests.kms_controller_key_trace_test_count()} production key trace retention, warnings, export and allocation-failure checks')

# Verify that task-only queue storage cannot silently return to the USB heap.
queue_before_dir = out / 'before-queue-psram'; queue_before_dir.mkdir(exist_ok=True)
input_production = (root / 'firmware/controller/main/usb_input.c').read_text()
queue_policy = ('QueueHandle_t queue = xQueueCreateWithCaps(count, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n'
                '    return queue ? queue : xQueueCreate(count, bytes);')
if input_production.count(queue_policy) != 1: raise SystemExit('Review USB queue placement regression mutation')
(queue_before_dir / 'usb_input.c').write_text(input_production.replace(queue_policy, 'return xQueueCreate(count, bytes);'))
queue_before_obj = compile_source(input_sources[0], '-before-queue-psram', queue_before_dir)
queue_before_dll = out / 'before-queue-psram.dll'
subprocess.run(compiler.link_command(
    queue_before_dll, [queue_before_obj, *[compile_source(source) for source in input_sources[1:]]]), check=True)
queue_before_tests = ctypes.CDLL(str(queue_before_dll))
failure = queue_before_tests.kms_run_usb_input_tests()
fixture_lines = (root / 'tests/native/test_usb_input.c').read_text().splitlines()
if not failure or not any(assertion in fixture_lines[failure - 1] for assertion in
    ('!internal_queue_bytes && external_queue_bytes == storage',
     'internal_queue_bytes == KMS_USB_QUEUE_DEPTH * sizeof(report_t) && external_queue_bytes > 0')):
    raise SystemExit('Previous internal queue placement was not rejected by the memory regression')
print('PASS: internal-only USB queue storage rejected by memory-placement regression')

console_obj = compile_source(root / 'tests/native/test_usb_console.c')
console_dll = out / 'usb-console-tests.dll'
subprocess.run(compiler.link_command(console_dll, [str(console_obj), str(out / 'runtime.obj')]), check=True)
console_tests = ctypes.CDLL(str(console_dll))
failure = console_tests.kms_run_usb_console_tests()
if failure:
    line = (root / 'tests/native/test_usb_console.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_console.c:{failure}: {line.strip()}')
print(f'PASS: {console_tests.kms_usb_console_test_count()} slow-console snapshot ownership, saturation and USB command dispatch checks')

fault_obj = compile_source(root / 'tests/native/test_usb_fault.c')
fault_model_obj = compile_source(root / 'firmware/components/kms_core/diagnostics_model.c')
fault_dll = out / 'usb-fault-tests.dll'
subprocess.run(compiler.link_command(
    fault_dll, [str(fault_obj), str(fault_model_obj), str(out / 'runtime.obj'), str(out / 'usb_health.obj')]), check=True)
fault_tests = ctypes.CDLL(str(fault_dll))
failure = fault_tests.kms_run_usb_fault_tests()
if failure:
    line = (root / 'tests/native/test_usb_fault.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_fault.c:{failure}: {line.strip()}')
print(f'PASS: {fault_tests.kms_usb_fault_test_count()} pre-recovery snapshot, control-result, frame-clock and read-only register checks; snapshot {fault_tests.kms_usb_fault_snapshot_bytes()} bytes')

driver_obj = compile_source(root / 'tests/native/test_usb_driver_trace.c')
driver_dll = out / 'usb-driver-tests.dll'
subprocess.run(compiler.link_command(driver_dll, [str(driver_obj), str(out / 'runtime.obj')]), check=True)
driver_tests = ctypes.CDLL(str(driver_dll))
failure = driver_tests.kms_run_usb_driver_tests()
if failure:
    line = (root / 'tests/native/test_usb_driver_trace.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_driver_trace.c:{failure}: {line.strip()}')
print(f'PASS: {driver_tests.kms_usb_driver_test_count()} USB HAL hook forwarding, pre-clear IRQ, hub-status filtering and trace-ring checks')

memory_obj = compile_source(root / 'tests/native/test_usb_memory.c')
memory_dll = out / 'usb-memory-tests.dll'
subprocess.run(compiler.link_command(memory_dll, [memory_obj, str(out / 'runtime.obj')]), check=True)
memory_tests = ctypes.CDLL(str(memory_dll))
failure = memory_tests.kms_run_usb_memory_tests()
if failure:
    line = (root / 'tests/native/test_usb_memory.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_memory.c:{failure}: {line.strip()}')
print(f'PASS: {memory_tests.kms_usb_memory_checks()} allocation hook correlation, ISR safety, first/last retention and snapshot checks')
