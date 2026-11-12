"""Run hardware-independent firmware tests with the native host C compiler.

Build a freestanding native shared library. The test harness supplies memory
primitives; the compiler runtime supplies stack probes for large test fixtures.
No Windows SDK or C runtime services are used.
"""
import ctypes
from pathlib import Path
import subprocess
import sys

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
compiler = NativeCompiler()
out = root / 'build/native-tests'
out.mkdir(parents=True, exist_ok=True)
core = root / 'firmware/components/kms_core'
sources = [core / n for n in ('log_buffer.c', 'jiggler.c', 'hid_parser.c', 'usb_policy.c', 'router.c', 'wire.c', 'controls.c', 'keyboard_leds.c', 'delivery.c', 'diagnostics_model.c', 'management.c', 'slave_history.c', 'slave_snapshot.c', 'user_preferences.c')]
sources += [root / 'tests/native/test_core.c', root / 'tests/native/runtime.c']
sources += [root / 'tests/native/test_recovery.c', root / 'tests/native/test_switching.c',
            core / 'ack.c', root / 'tests/native/test_ack.c']
sources += [root / 'tests/native/test_delivery_timing.c', root / 'tests/native/test_input_batch.c']
sources += [root / 'tests/native/test_shortcut_faults.c', root / 'tests/native/test_jiggler.c']
sources += [root / 'tests/native/test_jiggler_modes.c']
sources += [core / 'radio.c', root / 'tests/native/test_radio.c']
sources += [core / 'key_trace.c', root / 'tests/native/test_key_trace.c']
sources += [core / 'receiver_recovery.c', root / 'tests/native/test_receiver_recovery.c']
sources += [root / 'tests/native/test_management.c', root / 'tests/native/test_snapshot.c', root / 'tests/native/test_preferences.c',
            root / 'firmware/controller/main/preferences.c']
sources += [root / 'tests/native/test_usb_policy.c', root / 'tests/native/test_log_buffer.c']
sources += [core / 'firmware_cache.c', core / 'cached_update.c', core / 'stream.c', root / 'tests/native/test_firmware_cache.c']
sources += [root / 'firmware/controller/main/usb_leds.c', root / 'firmware/controller/main/usb_health.c', root / 'tests/native/test_usb_leds.c']
objects = []
for source in sources:
    obj = out / (source.stem + '.obj')
    subprocess.run(compiler.compile_command(
        source, obj,
        includes=[
            root / 'tests/native/include',
            core / 'include',
            root / 'config',
            root / 'firmware/controller/main',
            root / 'firmware/components/kms_runtime/include'],
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'kms-tests.dll'
subprocess.run(compiler.link_command(dll, objects, stack_probes=True), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_key_trace_tests()
if failure:
    line = (root / 'tests/native/test_key_trace.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_key_trace.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_key_trace_test_count()} key delivery trace, receipt and missing-key reason checks')
failure = tests.kms_run_receiver_recovery_tests()
if failure:
    line = (root / 'tests/native/test_receiver_recovery.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_receiver_recovery.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_receiver_recovery_test_count()} receiver recovery threshold, rolling budget, persistence and backoff checks')
for entry in ('kms_run_firmware_cache_tests', 'kms_run_cached_update_tests'):
    failure = getattr(tests, entry)()
    if failure:
        line = (root / 'tests/native/test_firmware_cache.c').read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL test_firmware_cache.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_firmware_cache_test_count()} cache interruption, corruption, local OTA and rollback checks')
failure = tests.kms_run_log_tests()
if failure:
    line = (root / 'tests/native/test_log_buffer.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_log_buffer.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_log_test_count()} PSRAM log retention, snapshot, ASCII and shortcut cancellation checks')
failure = tests.kms_run_radio_tests()
if failure:
    line = (root / 'tests/native/test_radio.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_radio.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_radio_test_count()} radio codec, discovery, OTA freeze and country-limit checks')
failure = tests.kms_run_tests()
if failure:
    line = (root / 'tests/native/test_core.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_core.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_test_count()} checks and 10,000 malformed HID descriptor cases')
failure = tests.kms_run_jiggler_tests()
if failure:
    line = (root / 'tests/native/test_jiggler.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_jiggler.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_jiggler_test_count()} jiggler timing, retry, switching and shortcut checks')
failure = tests.kms_run_jiggler_modes_tests()
if failure:
    line = (root / 'tests/native/test_jiggler_modes.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_jiggler_modes.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_jiggler_modes_test_count()} jiggler mode vectors, random timing, retries and per-dongle status checks')
failure = tests.kms_run_usb_led_tests()
if failure:
    line = (root / 'tests/native/test_usb_leds.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_leds.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_usb_led_test_count()} asynchronous USB LED checks (fake USB host; no hardware)')
failure = tests.kms_run_recovery_tests()
if failure:
    line = (root / 'tests/native/test_recovery.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_recovery.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_recovery_test_count()} recovery, replay and diagnostic checks (fake radio; no hardware)')
failure = tests.kms_run_ack_tests()
if failure:
    line = (root / 'tests/native/test_ack.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_ack.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_ack_test_count()} deferred ACK checks (busy transmitter; no hardware)')
failure = tests.kms_run_delivery_timing_tests()
if failure:
    line = (root / 'tests/native/test_delivery_timing.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_delivery_timing.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_delivery_timing_test_count()} asynchronous delivery latency, backlog, burst, duplicate and release checks')
failure = tests.kms_run_input_batch_tests()
if failure:
    line = (root / 'tests/native/test_input_batch.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_input_batch.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_input_batch_checks()} input batch codec, negotiation, ordering and restart checks')
failure = tests.kms_run_management_tests()
if failure:
    line = (root / 'tests/native/test_management.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_management.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_management_test_count()} management CRC, upload and bounded-history checks (fake flash; no hardware)')

failure = tests.kms_run_switching_tests()
if failure:
    line = (root / 'tests/native/test_switching.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_switching.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_switching_test_count()} shortcut/radio handover checks (two fake dongles; no hardware)')

failure = tests.kms_run_shortcut_fault_tests()
if failure:
    line = (root / 'tests/native/test_shortcut_faults.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_shortcut_faults.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_shortcut_fault_test_count()} shortcut fault-stage, timing and held-output safety checks')

failure = tests.kms_run_preferences_tests()
if failure:
    line = (root / 'tests/native/test_preferences.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_preferences.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_preferences_test_count()} preference codec, migration, persistence and runtime timing checks (fake NVS)')

failure = tests.kms_run_usb_policy_tests()
if failure:
    line = (root / 'tests/native/test_usb_policy.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_policy.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_usb_policy_test_count()} USB identity, selection and descriptor classification checks')

failure = tests.kms_run_snapshot_tests()
if failure:
    line = (root / 'tests/native/test_snapshot.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_snapshot.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_snapshot_test_count()} immutable chunk/CRC/maximum-size checks; snapshot RAM {tests.kms_snapshot_ram_bytes()} bytes')

# Compile the real history worker against deterministic queues and a fake NVS partition.
store_obj = out / 'test_diagnostic_store.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_diagnostic_store.c', store_obj,
    includes=[
        root / 'tests/native/history-store/include',
        root / 'tests/native/include',
        core / 'include',
        root / 'config',
        root / 'firmware/components/kms_runtime/include']), check=True)
store_dll = out / 'kms-store-tests.dll'
subprocess.run(compiler.link_command(
    store_dll, [str(store_obj), str(out / 'runtime.obj'), str(out / 'diagnostics_model.obj'), str(out / 'wire.obj')]), check=True)
store_tests = ctypes.CDLL(str(store_dll))
failure = store_tests.kms_run_store_tests()
if failure:
    line = (root / 'tests/native/test_diagnostic_store.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_diagnostic_store.c:{failure}: {line.strip()}')
print(f'PASS: {store_tests.kms_store_test_count()} production history worker queue/failure/reboot checks')

# Exercise the production radio coordinator with deterministic Wi-Fi/NVS/PSA fakes.
radio_obj = out / 'test_radio_channel.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_radio_channel.c', radio_obj,
    includes=[
        root / 'tests/native/radio/include',
            root / 'tests/native/usb-host/include',
        root / 'tests/native/include',
        core / 'include',
        root / 'config',
        root / 'firmware/components/kms_runtime/include'],
    disabled_warnings=[4244, 4267]), check=True)
radio_dll = out / 'kms-radio-tests.dll'
subprocess.run(compiler.link_command(
    radio_dll, [str(radio_obj), str(out / 'runtime.obj'), str(out / 'radio.obj')]), check=True)
radio_tests = ctypes.CDLL(str(radio_dll))
failure = radio_tests.kms_run_radio_channel_tests()
if failure:
    line = (root / 'tests/native/test_radio_channel.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_radio_channel.c:{failure}: {line.strip()}')
print(f'PASS: {radio_tests.kms_radio_channel_test_count()} production radio authentication, upgrade guard, persistence and retune checks')

# Run the production S2 USB serializer against a controllable TinyUSB host.
usb_device_obj = out / 'test_usb_device.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/test_usb_device.c', usb_device_obj,
    includes=[
        root / 'tests/native/usb-device/include',
        root / 'tests/native/include',
        core / 'include',
        root / 'config',
        root / 'firmware/components/kms_runtime/include'],
    disabled_warnings=[4244, 4267]), check=True)
usb_device_dll = out / 'kms-usb-device-tests.dll'
subprocess.run(compiler.link_command(
    usb_device_dll, [str(usb_device_obj), str(out / 'runtime.obj'), str(out / 'jiggler.obj'), str(out / 'wire.obj'), str(out / 'keyboard_leds.obj')]), check=True)
usb_device_tests = ctypes.CDLL(str(usb_device_dll))
failure = usb_device_tests.kms_run_usb_device_tests()
if failure:
    line = (root / 'tests/native/test_usb_device.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_usb_device.c:{failure}: {line.strip()}')
print(f'PASS: {usb_device_tests.kms_usb_device_test_count()} production S2 USB/jiggler held-state, busy-host and suspend checks')

subprocess.run([sys.executable, str(root / 'scripts/test-link-receive.py')], check=True)
subprocess.run([sys.executable, str(root / 'scripts/test-channel-recovery.py')], check=True)
subprocess.run([sys.executable, str(root / 'scripts/test-console-output.py')], check=True)
subprocess.run([sys.executable, str(root / "scripts/test-keyboard-console.py")], check=True)
subprocess.run([sys.executable, str(root / 'scripts/test-text.py')], check=True)
