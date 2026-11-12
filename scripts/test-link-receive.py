"""Exercise production ESP-NOW filtering and receiver selection.

Only callback dependencies are adapted. Queue item types and the callback body
are extracted verbatim from link.c; both wire codecs are compiled unchanged.
This verifies application filtering, not the closed-source Wi-Fi driver.
"""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
out = root / 'build/link-receive-tests'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'firmware/components/kms_runtime/link.c').read_text()
types = source[source.index('typedef struct { uint8_t mac[6]; int8_t rssi; kms_packet_t'):
               source.index('static const char *TAG')]
callback = source[source.index('static void receive(', source.index('static bool send_packet(')):source.index('static void save(')]
(out / 'link_receive.inc').write_text(types + '\n' + callback)
selection = source[source.index('bool kms_link_select('):source.index('static void pause_route(')]
peer_types = source[source.index('typedef struct { uint8_t valid, slot, mac[6]'):source.index('typedef struct { uint8_t mac[6]; int8_t rssi; kms_packet_t')]
(out / 'link_select_types.inc').write_text(peer_types)
(out / 'link_select.inc').write_text(selection)
route_starting = source[source.index('static bool route_starting('):source.index('static bool take_management_frame(')]
(out / 'link_route_starting.inc').write_text(route_starting)
transmit_types = source[source.index('typedef struct {\n    uint32_t started, submitted'):source.index('static uint32_t clipboard_deferred')]
(out / 'link_transmit_types.inc').write_text(transmit_types)
transmit = source[source.index('static esp_err_t configure_peer('):source.index('static void radio_receive_timing(')]
transmit += source[source.index('static bool send_frame('):source.index('static bool send_packet(')]
(out / 'link_transmit.inc').write_text(transmit)
(out / 'link_radio_status.inc').write_text(source[source.index('static void radio_receive_timing('):source.index('static bool send_frame(')])
scheduling = source[source.index('static bool take_management_frame('):source.index('static void management_tick(')]
(out / 'link_management.inc').write_text(scheduling)
unpair = source[source.index('const char *kms_link_unpair('):source.index('void kms_link_clear_input(')]
(out / 'link_unpair.inc').write_text(unpair)
emit = source[source.index('void kms_link_emit('):source.index('bool kms_link_select(')]
(out / 'link_emit.inc').write_text(emit)
(out / 'link_pause.inc').write_text(source[source.index('static void pause_route('):source.index('const char *kms_link_unpair(')])
(out / 'link_resume.inc').write_text(source[source.index('void kms_link_resume('):source.index('static void pair_close(')])
(out / 'link_recovery.inc').write_text(source[source.index('static struct { uint32_t sequence, kind, at, boot, reasons, attempts; } recovery_notices'):source.index('static void jiggler_tick(')])
(out / 'link_channel_idle.inc').write_text(source[source.index('static bool channel_probe_idle('):source.index('void kms_link_controller_recovery_tick(')])

compiler = NativeCompiler()
core = root / 'firmware/components/kms_core'
sources = [root / 'tests/native/test_link_receive.c', root / 'tests/native/test_link_transmit.c',
           root / 'tests/native/test_app.c', root / 'tests/native/runtime.c',
           core / 'wire.c', core / 'management.c', core / 'delivery.c', core / 'receiver_recovery.c']
objects = []
sources += [core / 'key_trace.c']
includes = [root / 'tests/native/include', core / 'include', root / 'config', out,
            root / 'tests/native/usb-host/include', root / 'firmware/components/kms_runtime/include']
for path in sources:
    obj = out / (path.stem + '.obj')
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=includes,
        disabled_warnings=[4244, 4267]), check=True)
    objects.append(str(obj))
dll = out / 'kms-link-receive-tests.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
tests = ctypes.CDLL(str(dll))
failure = tests.kms_run_app_tests()
if failure:
    line = (root / 'tests/native/test_app.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_app.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_app_test_count()} application wake/wait checks (fake task notifications)')
failure = tests.kms_run_link_receive_tests()
if failure:
    line = (root / 'tests/native/test_link_receive.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_link_receive.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_link_receive_test_count()} controller receive callback and selection checks')
receiver = out / 'test_receiver_input.obj'
subprocess.run(compiler.compile_command(sources[0], receiver, includes=includes,
                                       defines=['TEST_RECEIVER_INPUT'], disabled_warnings=[4244, 4267]), check=True)
receiver_dll = out / 'kms-receiver-input-tests.dll'
subprocess.run(compiler.link_command(receiver_dll, [str(receiver), *objects[1:]]), check=True)
receiver_tests = ctypes.CDLL(str(receiver_dll))
failure = receiver_tests.kms_run_link_receive_tests()
if failure:
    line = (root / 'tests/native/test_link_receive.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL receiver test_link_receive.c:{failure}: {line.strip()}')
print(f'PASS: {receiver_tests.kms_link_receive_test_count()} receiver receive callback checks, including full-size batches')
failure = tests.kms_run_link_transmit_tests()
if failure:
    line = (root / 'tests/native/test_link_transmit.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_link_transmit.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_link_transmit_test_count()} production transmit, stall recovery and clipboard priority checks')
failure = tests.kms_run_receiver_recovery_runtime_tests()
if failure:
    line = (root / 'tests/native/test_link_receive.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_link_receive.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_receiver_recovery_runtime_test_count()} production automatic recovery scheduling and route restoration checks')
failure = tests.kms_run_channel_route_tests()
if failure:
    line = (root / 'tests/native/test_link_receive.c').read_text().splitlines()[failure - 1]
    raise SystemExit(f'FAIL test_link_receive.c:{failure}: {line.strip()}')
print(f'PASS: {tests.kms_receiver_recovery_runtime_test_count()} channel trial route ownership and stale-input checks')
