"""Couple production USB lifecycle and pinned SDK allocation bodies to real TLSF."""
import ctypes
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
if len(sys.argv) == 3 and sys.argv[1] == '--run-dll':
    tests = ctypes.CDLL(sys.argv[2])
    failure = tests.kms_run_usb_shared_heap_tests()
    if failure:
        line = (root / 'tests/native/test_usb_shared_heap.c').read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL test_usb_shared_heap.c:{failure}: {line.strip()} (case={tests.kms_usb_shared_heap_case():08x}, TLSF assert={tests.kms_usb_shared_heap_error_line()})')
    print(f'PASS: {tests.kms_usb_shared_heap_checks()} shared TLSF checks across {tests.kms_usb_shared_heap_cases()} heap/layout cases')
    raise SystemExit(0)

# Also validates/extracts the unchanged pinned SDK function bodies and manifests.
sdk = runpy.run_path(str(root / 'scripts/test-usb-allocation.py'))
out, compiler = sdk['out'], sdk['compiler']
tlsf = sdk['idf'] / 'components/heap/tlsf'
tlsf_native = (tlsf / 'tlsf.c').read_text()
# Target metadata uses four-byte pointers. The SDK's dynamic metadata size
# omits padding before its pointer table; account for that native x64 gap.
padding = '(sizeof(*control->sl_bitmap) * control->fl_index_count)'
if tlsf_native.count(padding) != 1: raise SystemExit('Review TLSF native pointer-table padding')
tlsf_native = tlsf_native.replace(padding, '((sizeof(*control->sl_bitmap) * control->fl_index_count + sizeof(void*) - 1) & ~(sizeof(void*) - 1))')
for old, new, count in [('(void*)block +', '(char*)block +', 2),
                        ('block_to_ptr(block) + block_size(block)', '(char*)block_to_ptr(block) + block_size(block)', 1)]:
    if tlsf_native.count(old) != count: raise SystemExit('Review TLSF native pointer-arithmetic adapter')
    tlsf_native = tlsf_native.replace(old, new)
(out / 'sdk_tlsf_native.inc').write_text(tlsf_native.replace('status--;', 'status--; heap_error_line = __LINE__;'))
includes = [*sdk['includes'], tlsf, tlsf / 'include', root / 'firmware/controller/main',
            root / 'firmware/components/kms_core/include', root / 'firmware/components/kms_runtime/include', root / 'config']
warnings = [*sdk['warnings'], 4242, 4245, 4334]

def compile_source(path, name, extra_includes=(), defines=()):
    obj = out / (name + '.obj')
    subprocess.run(compiler.compile_command(
        path, obj,
        includes=[*extra_includes, *includes],
        defines=defines,
        disabled_warnings=warnings), check=True)
    return str(obj)

objects = [compile_source(root / 'tests/native/test_usb_shared_heap.c', 'shared-lifecycle'),
    compile_source(root / 'tests/native/test_usb_allocation.c', 'shared-sdk', defines=['KMS_USB_SHARED_HEAP']),
    compile_source(root / 'firmware/controller/main/usb_health.c', 'shared-health'),
    compile_source(root / 'firmware/components/kms_core/hid_parser.c', 'shared-parser'),
    compile_source(root / 'firmware/components/kms_core/usb_policy.c', 'shared-policy'), str(sdk['runtime'])]
dll = out / 'usb-shared-heap.dll'
subprocess.run(compiler.link_command(dll, objects), check=True)
result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(dll)], capture_output=True, text=True, timeout=30)
print(result.stdout, end='')
if result.returncode: raise SystemExit(result.stderr)
passed = result.stdout
before_dir = out / 'before-parser-compaction'; before_dir.mkdir(exist_ok=True)
production = (root / 'firmware/controller/main/usb_devices.c').read_text()
compaction = 'heap_caps_realloc(parser, kms_hid_parser_storage_size(parser), MALLOC_CAP_8BIT)'
if production.count(compaction) != 1: raise SystemExit('Review parser retention regression mutation')
# Reconstruct the previous full-size retention policy without depending on a
# feature-branch commit that may disappear from a fresh clone after squash merge.
# Keep endpoint-first ordering: this stronger baseline still fails on hardware.
before = production.replace(compaction, 'parser')
(before_dir / 'usb_devices.c').write_text(before)
before_obj = compile_source(root / 'tests/native/test_usb_shared_heap.c', 'before-shared-lifecycle', [before_dir])
before_dll = out / 'before-usb-shared-heap.dll'
subprocess.run(compiler.link_command(before_dll, [before_obj, *objects[1:]]), check=True)
rejected = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(before_dll)], capture_output=True, text=True, timeout=30)
if not rejected.returncode or 'pair_active(2) && pair_active(3)' not in rejected.stdout + rejected.stderr:
    raise SystemExit('Previous production allocation sequence was not rejected: ' + rejected.stdout + rejected.stderr)
print('PASS: previous full-size parser retention rejected by shared TLSF regression')
# Reintroduce the PSRAM-to-internal migration independently of compaction.
(before_dir / 'usb_devices.c').write_text(production.replace(compaction,
    'realloc(parser, kms_hid_parser_storage_size(parser))'))
psram_obj = compile_source(root / 'tests/native/test_usb_shared_heap.c', 'before-psram-shrink', [before_dir])
psram_dll = out / 'before-psram-shrink.dll'
subprocess.run(compiler.link_command(psram_dll, [psram_obj, *objects[1:]]), check=True)
migrated = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--run-dll', str(psram_dll)], capture_output=True, text=True, timeout=30)
if not migrated.returncode or not any(assertion in migrated.stdout + migrated.stderr for assertion in
    ('in_psram(interfaces[n].parser)', 'pair_active(2) && pair_active(3)')):
    raise SystemExit('PSRAM migration was not rejected: ' + migrated.stdout + migrated.stderr)
print('PASS: ordinary realloc PSRAM migration rejected by shared TLSF regression')
(out / 'shared-heap-before-after.json').write_text(json.dumps({'current': passed,
    'current_production_sha256': hashlib.sha256(production.encode()).hexdigest(),
    'previous_policy_sha256': hashlib.sha256(before.encode()).hexdigest(),
    'previous_policy': 'Retain the full parser; endpoint-first ordering retained to demonstrate that ordering alone is insufficient',
    'previous': rejected.stdout + rejected.stderr, 'psram_migration': migrated.stdout + migrated.stderr,
    'scenario': 'Internal fallback and PSRAM pools; two receivers; receiver reconnects'}, indent=2) + '\n')
(out / 'shared-heap-sources.json').write_text(json.dumps({
    'sources': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in tlsf.rglob('*') if p.is_file()},
    'adapters': ['Native bit-scan and optional weak hook replacements', 'Three GNU void-pointer arithmetic expressions cast to byte pointers',
                 'Account for x64 pointer-table alignment padding in TLSF control metadata', 'Integrity failure line reporting',
                 'Native x64 block headers and narrow HCD types', 'DMA allocation size rounded to requested alignment as in esp_heap_adjust_alignment_to_hw',
                 'Separate PSRAM pool; model internal-only default realloc migration versus capability-preserving shrink'],
    'limits': 'Actual TLSF algorithms with the listed native compatibility changes; no installed SDK source edits. Pools model DMA/default sharing, non-DMA fallback and PSRAM, not a byte-exact captured target heap or the complete ESP heap capability allocator.'
}, indent=2) + '\n')
