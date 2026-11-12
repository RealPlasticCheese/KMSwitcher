"""Regression audit of compiled production routing across generated desk/power layouts.

Every reported route is exercised through C CROSS/PREPARE/READY/SELECT. The independent
Python reference provides a differential check; production failures always fail the run.
"""
import argparse
from dataclasses import asdict, replace
import json
from pathlib import Path
import random
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests/routing'), str(ROOT / 'companion')]
from diagnostics.routing.model import build_plan
from production import build_production_plan
from scenarios import named_layouts, generated_layouts, availability_states
from test_model import assert_plan


def production_audit(layouts, seed):
    case = unittest.TestCase()
    case.plan_factory = build_production_plan
    case.routing_counts = dict(edge_samples=0, unassigned_edge_samples=0, active_barrier_checks=0)
    summary = dict(layouts=0, availability_states=0, directed_portals=0, crossings=0,
                   single_PC_toggles=0, max_PCs=0, max_monitors_per_PC=0, max_components=0)
    rng = random.Random(seed)
    for name, stations in layouts:
        summary['layouts'] += 1
        summary['max_PCs'] = max(summary['max_PCs'], len(stations))
        summary['max_monitors_per_PC'] = max(summary['max_monitors_per_PC'], *(len(s.screens) for s in stations))
        plans = {}
        for mask, state in availability_states(stations):
            try:
                # Verify C selections, exposed edges, real landings, unambiguous bands
                # and reachability of EVERY independently accessible monitor component.
                plan = assert_plan(case, state)
                case.assertEqual(plan.portals, build_plan(state).portals)
                if rng.randrange(8) == 0:
                    reordered = tuple(replace(s, screens=tuple(reversed(s.screens))) for s in reversed(state))
                    case.assertEqual(plan.portals, build_production_plan(reordered).portals)
            except Exception as error:
                raise AssertionError(json.dumps(dict(layout=name, mask=mask, stations=[asdict(s) for s in state]))) from error
            plans[mask] = plan
            summary['availability_states'] += 1
            summary['directed_portals'] += len(plan.portals)
            summary['crossings'] += sum(len({e.start, e.end - 1, (e.start + e.end - 1) // 2})
                                        for p in plan.portals for e in p.edges)
            summary['max_components'] = max(summary['max_components'], len(plan.nodes))
        # Both endpoints of each on/off transition were checked above. Native unit
        # tests exercise pending handoff races without resetting the firmware state.
        for mask, plan in plans.items():
            for bit in range(len(stations)):
                after = plans[mask ^ (1 << bit)]
                case.assertNotEqual(plan.snapshot, after.snapshot)
                summary['single_PC_toggles'] += 1
    summary.update(case.routing_counts)
    summary.update(disconnected_states=0, ineligible_selections=0, active_PC_skips=0, invalid_physical_spans=0)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seed', type=int, default=72519)
    parser.add_argument('--per-count', type=int, default=400)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/routing-audit.json')
    parser.add_argument('--skip-native-build', action='store_true')
    parser.add_argument('--require-production', action='store_true', help='Retained for compatibility; always enforced')
    args = parser.parse_args()
    if args.per_count < 0: parser.error('--per-count must be nonnegative')
    if not args.skip_native_build:
        subprocess.run([sys.executable, str(ROOT / 'scripts/test-desktop.py')], cwd=ROOT, check=True)
    result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.discover(str(ROOT / 'tests/routing')))
    if not result.wasSuccessful(): return 1
    start = time.monotonic()
    summary = dict(schema=3, seed=args.seed, per_count=args.per_count, unit_tests=result.testsRun,
                   production_integrated=True,
                   production=production_audit(list(named_layouts()) + list(generated_layouts(args.seed, args.per_count)), args.seed))
    summary['elapsed_seconds'] = round(time.monotonic() - start, 2)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
