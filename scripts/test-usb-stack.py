"""Test the pinned ESP-IDF S3 USB HAL/LL algorithms against RAM registers.

Requires the installed repository toolchain. This does not emulate DMA, W1C
hardware side effects, USB wire traffic, the HCD scheduler or RTOS timing.
"""
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from native_build import NativeCompiler, sdk_path

if len(sys.argv) == 3 and sys.argv[1] == '--run-guard-dll':
    tests = ctypes.CDLL(sys.argv[2])
    failure = tests.kms_run_guard_hal_tests()
    if failure:
        fixture = Path(__file__).resolve().parents[1] / 'tests/native/test_usb_frame_guard_hal.c'
        line = fixture.read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL guarded HAL:{failure}: {line.strip()}')
    print(f'PASS: {tests.kms_guard_hal_check_count()} production guarded-HAL checks across {tests.kms_guard_hal_case_count()} cases')
    raise SystemExit(0)

if len(sys.argv) == 3 and sys.argv[1] == '--run-dll':
    tests = ctypes.CDLL(sys.argv[2])
    failure = tests.kms_run_usb_stack_tests()
    if failure:
        fixture = Path(__file__).resolve().parents[1] / 'tests/native/test_usb_stack.c'
        line = fixture.read_text().splitlines()[failure - 1]
        detail = f'case={tests.kms_usb_stack_case():08x} actual={tests.kms_usb_stack_actual():x} expected={tests.kms_usb_stack_expected():x}'
        raise SystemExit(f'FAIL test_usb_stack.c:{failure}: {line.strip()} ({detail})')
    print(f'PASS: {tests.kms_usb_stack_test_count()} actual S3 HAL/LL interrupt and register checks')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
common = Path(subprocess.check_output(['git', '-C', str(root), 'rev-parse',
    '--path-format=absolute', '--git-common-dir'], text=True).strip()).parent
version = json.loads((root / 'toolchain.json').read_text())['espIdfVersion']
idf = sdk_path(root)
hal = idf / 'components/esp_hal_usb'
source = hal / 'usb_dwc_hal.c'
if not source.is_file():
    raise SystemExit('Install and activate the pinned toolchain first.')

compiler = NativeCompiler()
out = root / 'build/usb-stack-tests'
out.mkdir(parents=True, exist_ok=True)
native_headers = out / 'include/hal'
native_headers.mkdir(parents=True, exist_ok=True)
hal_header = (hal / 'include/hal/usb_dwc_hal.h').read_text()
field = 'usb_dwc_xfer_type_t type: 2;'
if hal_header.count(field) != 1:
    raise SystemExit('Review the native enum-bitfield ABI adapter for this SDK version.')
# GCC gives this nonnegative enum an unsigned underlying type; MSVC uses signed
# int, turning interrupt type 3 in a two-bit field into -1. Adapt representation
# only. HAL/LL algorithms and the SoC register definitions remain unmodified.
native_header = hal_header.replace(field, 'unsigned int type: 2;')
(native_headers / 'usb_dwc_hal.h').write_text(native_header)
includes = [out / 'include', root / 'tests/native/usb-stack/include', root / 'tests/native/include',
    hal / 'include', hal / 'esp32s3/include', idf / 'components/soc/esp32s3/include']
objects = []
warnings = [4244, 4267, 4302, 4311, 4312, 4100, 4245, 4018, 4389]
forced = [root / 'tests/native/usb-stack/include/compat.h']
for path in [source, root / 'tests/native/test_usb_stack.c', root / 'tests/native/runtime.c']:
    obj = out / (path.stem + '.obj')
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=includes,
        disabled_warnings=warnings,
        force_include=forced), check=True)
    objects.append(str(obj))
dll = out / 'usb-stack-tests.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(dll)], check=True, timeout=20)
files = [source, hal / 'include/hal/usb_dwc_hal.h', hal / 'esp32s3/include/hal/usb_dwc_ll.h',
    idf / 'components/soc/esp32s3/include/soc/usb_dwc_struct.h']
manifest = {'idf': version, 'files': {str(p.relative_to(idf)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
manifest['native_abi_adapter'] = 'usb_dwc_hal_ep_char_t.type: unsigned int replaces enum in the two-bit field; matches GCC signedness'
(out / 'dependencies.lock').write_bytes((root / 'firmware/controller/dependencies.lock').read_bytes())
manifest['usb_component_lock_sha256'] = hashlib.sha256((out / 'dependencies.lock').read_bytes()).hexdigest()
(out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Source SHA-256 manifest: ' + str(out / 'source-manifest.json'))

# Verify that the matrix is sensitive to regressions in upstream algorithms.
# These isolated broken copies never replace the installed SDK or build inputs.
original = source.read_text()
halt = 'usb_dwc_ll_hcchar_disable_chan(chan_obj->regs);'
assert original.count(halt) == 2
short_packet = '/* missing short-packet halt */'.join(original.rsplit(halt, 1))
pending = 'hal->channels.chan_pend_intrs_msk &= ~(1 << (chan_num - 1));'
errors = 'if (chan_intrs & CHAN_INTRS_ERROR_MSK)'
assert original.count(pending) == original.count(errors) == 1
mutations = [
    ('missing-short-packet-halt', short_packet, 'c->flags.active && c->regs->hcchar_reg.chdis'),
    ('lost-simultaneous-interrupt', original.replace(pending, 'hal->channels.chan_pend_intrs_msk = 0;'), 'seen == mask'),
    ('completion-before-error', original.replace(errors, 'if ((chan_intrs & CHAN_INTRS_ERROR_MSK) && !(chan_intrs & USB_DWC_LL_INTR_CHAN_CHHLTD))'), 'USB_DWC_HAL_CHAN_EVENT_ERROR')
]
for name, content, expected in mutations:
    path = out / (name + '.c'); path.write_text(content)
    obj = out / (name + '.obj')
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=includes,
        disabled_warnings=warnings,
        force_include=forced), check=True)
    dll = out / (name + '.dll')
    subprocess.run(compiler.link_command(dll, [str(obj), *objects[1:]]), check=True)
    result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(dll)],
                            capture_output=True, text=True, timeout=20)
    output = result.stdout + result.stderr
    if result.returncode == 0 or expected not in output:
        raise SystemExit(f'FAIL: mutation {name} was not rejected as expected: {output}')
    print(f'PASS: isolated SDK mutation {name} rejected')

# Exercise the actual generated production HAL and shared guard together. These
# RAM registers validate endpoint scoping/SDK ownership, not physical bus timing.
guard_dir = root / 'firmware/usb_frame_guard'
spec = importlib.util.spec_from_file_location('guard_hal', guard_dir / 'instrument_hal.py')
guard_hal = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard_hal)
guarded = guard_hal.instrument(source.read_bytes())
guard_includes = [root / 'tests/native/frame-guard/include', guard_dir, *includes]
guard_objects = []
for path in (guard_dir / 'kms_usb_frame_guard.c', root / 'tests/native/test_usb_frame_guard_hal.c'):
    obj = out / (path.stem + '.obj')
    extra = [root / 'tests/native/stress/include'] if path.name == 'kms_usb_frame_guard.c' else []
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=[*extra, *guard_includes],
        disabled_warnings=warnings,
        force_include=forced), check=True)
    guard_objects.append(str(obj))
for name, content, expected_failure in (
    ('guarded', guarded, None),
    ('guard-missing', guarded.replace('kms_usb_frame_guard_before_enable(kms_guard_interrupt_in);', '(void)kms_guard_interrupt_in;'), 'stats.enabled==enabled && stats.activations==eligible'),
    ('guard-wrong-endpoints', guarded.replace('chan_obj->type == USB_DWC_XFER_TYPE_INTR && chan_obj->regs->hcchar_reg.epdir', 'true'), 'stats.enabled==enabled && stats.activations==eligible'),
):
    path = out / (name + '.c'); path.write_text(content)
    obj = out / (name + '.obj')
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=guard_includes,
        disabled_warnings=warnings,
        force_include=forced), check=True)
    dll = out / (name + '.dll')
    subprocess.run(compiler.link_command(dll, [str(obj), *guard_objects, objects[-1]]), check=True)
    result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-guard-dll', str(dll)],
                            capture_output=True, text=True, timeout=20)
    output = result.stdout + result.stderr
    if expected_failure:
        if not result.returncode or expected_failure not in output:
            raise SystemExit(f'FAIL: production guard mutation {name} was not rejected: {output}')
        print(f'PASS: production guard mutation {name} rejected')
    else:
        if result.returncode: raise SystemExit(output)
        print(output.strip())
manifest['guard_overlay_sha256'] = hashlib.sha256(guarded.encode()).hexdigest()
manifest['guard_source_sha256'] = hashlib.sha256((guard_dir / 'kms_usb_frame_guard.c').read_bytes()).hexdigest()
(out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
