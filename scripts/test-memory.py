"""Check task-memory allocation policy and optional linked S3 storage budgets."""
import argparse
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--controller-elf', type=Path)
parser.add_argument('--receiver-elf', type=Path)
parser.add_argument('--elf-only', action='store_true')
args = parser.parse_args()
if not args.elf_only:
    compiler = NativeCompiler()
    out = root / 'build/memory-tests'; out.mkdir(parents=True, exist_ok=True)
    includes = [root / 'tests/native/memory/include', root / 'tests/native/usb-host/include',
                root / 'tests/native/include', root / 'firmware/components/kms_runtime/include']
    runtime = out / 'runtime.obj'
    subprocess.run(compiler.compile_command(root / 'tests/native/runtime.c', runtime, includes=includes), check=True)
    for name, s3, psram in [('s3-psram', 1, 1), ('s3-internal', 1, 0), ('s2-internal', 0, 0)]:
        obj, dll = out / (name + '.obj'), out / (name + '.dll')
        subprocess.run(compiler.compile_command(
            root / 'tests/native/test_memory.c', obj,
            includes=includes,
            defines=[f'CONFIG_IDF_TARGET_ESP32S3={s3}', f'CONFIG_SPIRAM={psram}']), check=True)
        subprocess.run(compiler.link_command(dll, [str(obj), str(runtime)]), check=True)
        tests = ctypes.CDLL(str(dll)); failure = tests.kms_run_memory_tests()
        if failure:
            line = (root / 'tests/native/test_memory.c').read_text().splitlines()[failure - 1]
            raise SystemExit(f'FAIL {name} test_memory.c:{failure}: {line.strip()}')
        print(f'PASS: {name}: {tests.kms_memory_checks()} allocation/fallback/queue-cleanup checks')
if args.elf_only and not (args.controller_elf or args.receiver_elf):
    parser.error('--elf-only requires --controller-elf or --receiver-elf')


def linked_objects(path):
    # pyelftools is supplied by the pinned ESP-IDF Python environment.
    from elftools.elf.elffile import ELFFile
    objects = []
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        source = ''
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol['st_info']['type'] == 'STT_FILE':
                source = symbol.name.replace('\\', '/').split('/')[-1]
            if not isinstance(symbol['st_shndx'], int):
                continue
            if symbol.name in ('tcpip_init', 'tcpip_thread', 'esp_netif_init'):
                raise SystemExit(f'FAIL: {path.name} retains unused IP startup symbol {symbol.name}')
            if symbol['st_info']['type'] != 'STT_OBJECT':
                continue
            if elf.get_section(symbol['st_shndx']).name in ('.dram0.bss', '.dram0.data'):
                objects.append((source, symbol.name, symbol['st_size']))
    return objects


def check_budget(objects, source, limit, name=None, minimum=0):
    used = sum(size for file, symbol, size in objects if file == source and (name is None or symbol == name))
    label = source if name is None else f'{source}/{name}'
    if not minimum <= used <= limit:
        raise SystemExit(f'FAIL: {label} retains {used} internal static bytes (expected {minimum} to {limit})')
    print(f'PASS: {label}: {used} internal static bytes (budget {limit})')


if args.controller_elf:
    controller = linked_objects(args.controller_elf)
    budgets = {'diagnostic_store.c': 1024, 'usb_console.c': 1024, 'slave_diagnostics.c': 256,
               'stream_radio.c': 1200, 'stream_runtime.c': 64, 'desktop_routes.c': 0, 'usb_fault.c': 16}
    for source, limit in budgets.items():
        check_budget(controller, source, limit)
    check_budget(controller, 'desktop_runtime.c', 4, 'state', minimum=4)
    check_budget(controller, 'link.c', 1600, 'sender', minimum=1)
    for name in ('receipt', 'text_receipt', 'text_rx'):
        check_budget(controller, 'link.c', 0, name)
if args.receiver_elf:
    receiver = linked_objects(args.receiver_elf)
    check_budget(receiver, 'link.c', 0, 'sender')
    check_budget(receiver, 'desktop_runtime.c', 4, 'state')
    check_budget(receiver, 'remote.c', 0, 'peers')
    for name in ('receipt', 'text_receipt', 'text_rx'):
        check_budget(receiver, 'link.c', 160, name, minimum=1)
    check_budget(receiver, 'slave_diagnostics.c', 11000, 'history', minimum=10000)
