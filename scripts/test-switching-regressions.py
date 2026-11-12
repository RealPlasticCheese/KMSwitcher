"""Run native tests and check shutdown regressions against isolated broken copies.

Each negative control changes one behavior in the current implementation.
"""
import ctypes
from pathlib import Path
import runpy
import subprocess

root = Path(__file__).resolve().parents[1]
ctx = runpy.run_path(str(root / 'scripts/test-core.py'))
out = ctx['out'] / 'switching-regressions'
out.mkdir(exist_ok=True)
core = ctx['core']
router = (core / 'router.c').read_text()
delivery = (core / 'delivery.c').read_text()
preserve_shortcut = "if (r->inputs[n].tap_phase || (r->mode && r->owner == n)) r->inputs[n].tap_replay_blocked = true;"
reset_shortcut = """r->inputs[n].tap_phase = TAP_NONE;
        r->inputs[n].tap_replay_blocked = false;
        r->mode = KMS_MODE_NORMAL;"""
assert router.count(preserve_shortcut) == 1
clear_route = "d->need_activate = d->have_status = d->block = false;"
assert delivery.count(clear_route) == 1

def reject(name, source, stem, run, fixture, expected):
    path = out / f'{name}.c'
    path.write_text(source)
    obj = out / f'{name}.obj'
    subprocess.run(ctx['compiler'].compile_command(
        path, obj,
        includes=[root / 'tests/native/include', core / 'include', root / 'config']), check=True)
    objects = [str(obj) if Path(p).stem == stem else p for p in ctx['objects']]
    dll = out / f'{name}.dll'
    subprocess.run(ctx['compiler'].link_command(dll, objects, stack_probes=True), check=True)
    tests = ctypes.CDLL(str(dll))
    failure = getattr(tests, run)()
    line = (root / 'tests/native' / fixture).read_text().splitlines()[failure - 1] if failure else ''
    if not failure or expected not in line:
        raise SystemExit(f'FAIL {name}: expected {expected!r}, got {line!r}')
    print(f'PASS: {name} rejected by {fixture}:{failure}: {line.strip()}')

reject('reset-shortcut-on-output-block', router.replace(preserve_shortcut, reset_shortcut), 'router', 'kms_run_switching_tests',
       'test_switching.c', 'sender.slot == 2 && peers[1].selections == 1')
reject('retain-obsolete-route-block', delivery.replace(clear_route, 'd->need_activate = d->have_status = false;'), 'delivery', 'kms_run_switching_tests',
       'test_switching.c', 'CHECK(!sender.block)')
guard = '    if (i->tap_replay_blocked) return;'
assert router.count(guard) == 1
reject('missing-prefix-replay-protection', router.replace(guard, ''), 'router',
       'kms_run_shortcut_fault_tests', 'test_shortcut_faults.c', 'neutral(&observed)')
print('PASS: current production code passes; all three regressions fail independently')
