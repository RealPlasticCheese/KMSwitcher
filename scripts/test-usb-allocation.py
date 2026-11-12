"""Run selected pinned USBH/HCD allocator functions with deterministic native heaps.

SDK function bodies are copied verbatim into build output. Types, heap, channel,
cache and list operations are native adapters, not a full HCD/RTOS/DMA emulator.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from native_build import NativeCompiler, sdk_path

root = Path(__file__).resolve().parents[1]
if len(sys.argv) == 3 and sys.argv[1] == '--run-dll':
    tests = ctypes.CDLL(sys.argv[2])
    failure = tests.kms_run_usb_allocation_tests()
    if failure:
        line = (root / 'tests/native/test_usb_allocation.c').read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL test_usb_allocation.c:{failure}: {line.strip()} (case={tests.kms_usb_allocation_case() & 0xffffffff:08x})')
    print(f'PASS: {tests.kms_usb_allocation_checks()} SDK USBH/HCD allocation checks across {tests.kms_usb_allocation_cases()} matrix cases')
    raise SystemExit(0)

common = Path(subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'], text=True).strip()).parent
component = root / 'firmware/controller/managed_components/espressif__usb'
if not component.is_dir(): component = common / 'firmware/controller/managed_components/espressif__usb'
if not component.is_dir(): raise SystemExit('Install the pinned USB component with a controller configure/build first')
lock = (root / 'firmware/controller/dependencies.lock').read_bytes()
def usb_lock(text):
    match = re.search(r'(?m)^  espressif/usb:\n.*?(?=^  \S|\Z)', text, re.S)
    if not match: raise SystemExit('Missing locked espressif/usb component')
    return match[0].strip()
if usb_lock(lock.decode().replace('\r\n', '\n')) != usb_lock((component.parent.parent / 'dependencies.lock').read_text()):
    raise SystemExit('The installed USB component lock differs from this worktree; configure its controller first')
version = json.loads((root / 'toolchain.json').read_text())['espIdfVersion']
idf = sdk_path(root)
ll_path = idf / 'components/esp_hal_usb/esp32s3/include/hal/usb_dwc_ll.h'
hcd_path, usbh_path = [component / 'src' / p for p in ('hcd_dwc.c', 'usbh.c')]
hcd, usbh, ll = [p.read_text() for p in (hcd_path, usbh_path, ll_path)]

def function(text, name):
    match = re.search(r'(?m)^(?:static )?[\w *]+\b' + name + r'\([^;{}]*\)\n\{', text)
    if not match: raise SystemExit(f'Review SDK allocation function extraction: {name}')
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]', text[match.end() - 1:], re.S):
        if token[0] == '{': depth += 1
        elif token[0] == '}':
            depth -= 1
            if not depth: return text[match.start():match.end() - 1 + token.end()]
    raise SystemExit('Unbalanced SDK function: ' + name)

names = ('buffer_block_alloc', 'buffer_block_free', 'hcd_pipe_alloc', 'hcd_pipe_free', 'endpoint_alloc', 'endpoint_free')
functions = {name: function(hcd if name in names[:4] else usbh, name) for name in names}
out = root / 'build/usb-allocation-tests'; out.mkdir(parents=True, exist_ok=True)
generated = out / 'sdk_allocation_functions.inc'
generated.write_text('\n\n'.join(functions.values()) + '\n')
constants = []
for name, value in (('XFER_LIST_LEN_CTRL', '3'), ('XFER_LIST_LEN_BULK', '2'),
                    ('XFER_LIST_LEN_ISOC', '64'), ('XFER_LIST_LEN_INTR', 'FRAME_LIST_LEN')):
    if not re.search(r'^#define ' + name + r'\s+' + value + r'\b', hcd, re.M):
        raise SystemExit('Review changed SDK allocation constant: ' + name)
    constants.append(f'#define {name} {"32" if value == "FRAME_LIST_LEN" else value}')
if not re.search(r'^#define FRAME_LIST_LEN\s+USB_HAL_FRAME_LIST_LEN_32\b', hcd, re.M):
    raise SystemExit('Review changed SDK frame-list size')
if not re.search(r'^#define USB_DWC_QTD_LIST_MEM_ALIGN\s+512\b', ll, re.M):
    raise SystemExit('Review changed SDK QTD alignment')
constants.append('#define USB_DWC_QTD_LIST_MEM_ALIGN 512')
end = ll.index('} usb_dwc_ll_dma_qtd_t;') + len('} usb_dwc_ll_dma_qtd_t;')
qtd = ll[ll.rfind('typedef struct {', 0, end):end]
if qtd.count('uint8_t *buffer;') != 1: raise SystemExit('Review target-QTD native pointer adapter')
constants.append(qtd.replace('uint8_t *buffer;', 'uint32_t buffer;'))
(out / 'sdk_allocation_constants.inc').write_text('\n'.join(constants) + '\n')
manifest = {'idf': version, 'component_lock_sha256': hashlib.sha256(lock).hexdigest(),
            'sources': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (hcd_path, usbh_path, ll_path)},
            'functions': {name: hashlib.sha256(body.encode()).hexdigest() for name, body in functions.items()},
            'adapters': ['Narrow native port/pipe/endpoint types; selected allocation functions only',
                         'QTD buffer pointer represented as uint32 to retain S3 8-byte descriptor size on x64',
                         'Deterministic separate default/DMA heaps, 512-byte alignment, fake channel/cache/list primitives'],
            'limits': 'No live heap capture, TLSF implementation, full HCD scheduler, DMA or FreeRTOS emulation'}
(out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
(out / 'dependencies.lock').write_bytes(lock)

compiler = NativeCompiler()
includes = [out, root / 'tests/native/usb-host/include', root / 'tests/native/include']
warnings = [4244, 4267, 4018, 4389]
runtime = out / 'runtime.obj'
subprocess.run(compiler.compile_command(
    root / 'tests/native/runtime.c', runtime,
    includes=includes,
    disabled_warnings=warnings), check=True)

def run(name):
    obj, dll = out / (name + '.obj'), out / (name + '.dll')
    subprocess.run(compiler.compile_command(
        root / 'tests/native/test_usb_allocation.c', obj,
        includes=includes,
        disabled_warnings=warnings), check=True)
    subprocess.run(compiler.link_command(dll, [str(obj), str(runtime)]), check=True)
    return subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(dll)], capture_output=True, text=True, timeout=30)

result = run('usb-allocation-tests')
print(result.stdout, end='')
if result.returncode: raise SystemExit(result.stderr)
passed_output = result.stdout
original = generated.read_text()
mutations = [
    ('leaked-dma-list', 'heap_caps_free(buffer->xfer_desc_list);', '/* leaked DMA list */', '!live()'),
    ('leaked-pipe-error-buffers', 'buffer_block_free(buffers[i]);', '/* leaked partial buffers */', '!live()'),
    ('leaked-endpoint-on-pipe-error', 'pipe_err:\n    heap_caps_free(ep_obj);', 'pipe_err:\n    /* leaked endpoint */', '!live()'),
    ('missing-alignment-check', '(xfer_desc_list == NULL) || ((uintptr_t)xfer_desc_list & (USB_DWC_QTD_LIST_MEM_ALIGN - 1))', 'xfer_desc_list == NULL', 'result == ESP_ERR_NO_MEM'),
]
try:
    for name, old, new, expected in mutations:
        if original.count(old) != 1: raise SystemExit('Review SDK allocation mutation: ' + name)
        generated.write_text(original.replace(old, new))
        result = run(name)
        if not result.returncode or expected not in result.stdout + result.stderr:
            raise SystemExit(f'Allocation mutation {name} was not rejected: {result.stdout}{result.stderr}')
        print(f'PASS: isolated SDK allocation mutation {name} rejected')
finally:
    generated.write_text(original)
checks, cases = map(int, re.search(r'PASS: (\d+).*across (\d+) matrix cases', passed_output).groups())
(out / 'matrix-report.json').write_text(json.dumps({'status': 'PASS', 'cases': cases, 'checks': checks,
    'axes': {'transfer_types': 4, 'occupied_channels': list(range(9)), 'allocation_failure_points': list(range(8)),
             'invalid_dma_alignment': [False, True], 'cache_alignment': [0, 32],
             'fragment_pinning_offsets': list(range(0, 256, 17)), 'fragment_strides': [256, 512],
             'churn': '1-8 real allocated pipes; each replacement slot; every failure point; 8 cycles',
             'disconnect_during_allocation': list(range(7))},
    'mutations_rejected': [item[0] for item in mutations], 'source_manifest': 'source-manifest.json',
    'finding': 'Persistent aligned-DMA pressure reproduced in the model, without a source leak; live-incident cause unconfirmed'
}, indent=2) + '\n')
print('Source SHA-256 manifest: ' + str(out / 'source-manifest.json'))
