from dataclasses import replace
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'companion'))
from kms_companion.geometry import Rect as HelperRect, crossing as helper_crossing
from diagnostics.routing.model import (LEFT, RIGHT, UP, DOWN, Rect, Station,
    build_plan, UnsupportedLayout, components, exposed_edges, Plan, Edge)
from scenarios import user_layout, named_layouts


def reachable(graph, start):
    seen, pending = {start}, [start]
    while pending:
        for target in graph[pending.pop()] - seen:
            seen.add(target)
            pending.append(target)
    return seen


def assert_plan(test, stations, corner=8):
    plan = getattr(test, "plan_factory", build_plan)(stations, corner)
    return validate_plan(test, stations, plan, corner)


def validate_plan(test, stations, plan, corner=8):
    active = {s.slot for s in stations if s.available}
    test.assertEqual({n.slot for n in plan.nodes}, active)
    graph = {n.key: set() for n in plan.nodes}
    used = {}
    for portal in plan.portals:
        test.assertNotEqual(portal.source[0], portal.target[0])
        test.assertIn(portal.target[0], active)
        test.assertGreater(portal.length, 0)
        source = next(n for n in plan.nodes if n.key == portal.source)
        destination = next(n for n in plan.nodes if n.key == portal.target)
        screens = [HelperRect(r.x, r.y, r.width, r.height) for r in source.screens]
        observed = False
        for edge in portal.edges:
            for along in {edge.start, edge.end - 1, (edge.start + edge.end - 1) // 2}:
                x, y = edge.point(along)
                motion = ((-1, 0), (1, 0), (0, -1), (0, 1))[edge.direction]
                test.assertEqual(helper_crossing(screens, (x, y), motion, corner), edge.direction)
                result = plan.cross(source.slot, edge.direction, x, y)
                test.assertIsNotNone(result)
                test.assertEqual(result.target, destination.key)
                test.assertTrue(any(r.contains(*result.landing) for r in destination.screens))
                test.assertTrue(plan.accepts(result))
                observed = True
            key = (source.key, edge.direction, edge.fixed)
            for previous in used.get(key, []):
                test.assertFalse(max(previous.start, edge.start) < min(previous.end, edge.end),
                                 'Overlapping edge bands make one destination unreachable')
            used.setdefault(key, []).append(edge)
        test.assertTrue(observed)
        graph[portal.source].add(portal.target)
        test.assertTrue(any(p.source == portal.target and p.target == portal.source for p in plan.portals))
    if len(active) > 1:
        # Check actual usable directed portals, not just the planner's intended links.
        # Monitor components are vertices: PC-level connectivity alone can conceal traps.
        for start in graph:
            test.assertEqual(reachable(graph, start), set(graph))
    else:
        test.assertFalse(plan.portals)
    counts = assert_edge_boundaries(test, stations, plan, corner)
    if hasattr(test, 'routing_counts'):
        for key, value in counts.items(): test.routing_counts[key] += value
    return plan


def assert_edge_boundaries(test, stations, plan, corner):
    """Independent negative oracle: check unassigned spans and awake-PC barriers."""
    active = [s for s in stations if s.available]
    counts = dict(edge_samples=0, unassigned_edge_samples=0, active_barrier_checks=0)
    for source in plan.nodes:
        outgoing = [p for p in plan.portals if p.source == source.key]
        for edge in exposed_edges(source, corner):
            cuts = {edge.start, edge.end}
            for s in active:
                for r in s.screens:
                    for value in (r.y, r.bottom) if edge.direction < UP else (r.x, r.right):
                        if edge.start < value < edge.end: cuts.add(value)
            for p in outgoing:
                for band in p.edges:
                    if band.direction == edge.direction and band.fixed == edge.fixed:
                        cuts.update(v for v in (band.start, band.end) if edge.start < v < edge.end)
            ordered = sorted(cuts)
            for lo, hi in zip(ordered, ordered[1:]):
                for along in {lo, hi - 1, (lo + hi - 1) // 2}:
                    x, y = edge.point(along)
                    assigned = next((p for p in outgoing if any(e.includes(edge.direction, x, y) for e in p.edges)), None)
                    result = plan.cross(source.slot, edge.direction, x, y)
                    counts['edge_samples'] += 1
                    if assigned is None:
                        counts['unassigned_edge_samples'] += 1
                        test.assertIsNone(result, 'An unassigned edge must not switch PCs')
                    else:
                        test.assertIsNotNone(result)
                        test.assertEqual(result.target, assigned.target)
                    touching = set()
                    for s in active:
                        if s.slot == source.slot or any(r.contains(x, y) for r in s.screens): continue
                        if any((edge.direction == RIGHT and r.x == x + 1 and r.y <= y < r.bottom) or
                               (edge.direction == LEFT and r.right == x and r.y <= y < r.bottom) or
                               (edge.direction == DOWN and r.y == y + 1 and r.x <= x < r.right) or
                               (edge.direction == UP and r.bottom == y and r.x <= x < r.right) for r in s.screens):
                            touching.add(s.slot)
                    if touching:
                        test.assertIsNotNone(result, 'A physical shared edge must keep working')
                        test.assertIn(result.target[0], touching, 'A temporary route must not override an active physical neighbour')
                    if assigned is not None and not assigned.virtual:
                        test.assertIn(result.target[0], touching, 'A physical route must stay within its actual shared monitor span')
                    if result is None: continue
                    target = next(s for s in active if s.slot == result.target[0])
                    def distances(station):
                        found = []
                        plane, transverse = (x + (edge.direction == RIGHT), y) if edge.direction < UP else (y + (edge.direction == DOWN), x)
                        for r in station.screens:
                            low, high = (r.x, r.right) if edge.direction < UP else (r.y, r.bottom)
                            a, b = (r.y, r.bottom) if edge.direction < UP else (r.x, r.right)
                            if not a <= transverse < b: continue
                            if edge.direction in (RIGHT, DOWN) and high > plane: found.append(max(0, low - plane))
                            if edge.direction in (LEFT, UP) and low < plane: found.append(max(0, plane - high))
                        return found
                    target_distances = distances(target)
                    for s in active:
                        if s.slot in (source.slot, target.slot): continue
                        blockers = distances(s)
                        if blockers:
                            counts['active_barrier_checks'] += 1
                            test.assertTrue(target_distances and min(target_distances) <= min(blockers),
                                            'A crossing must not jump past a nearer active PC')
    return counts


class RoutingModelTests(unittest.TestCase):
    plan_factory = staticmethod(build_plan)
    def test_named_layouts_and_all_power_subsets(self):
        from scenarios import availability_states
        for name, stations in named_layouts():
            # The 112 disconnected-component stress case is expensive; all 128
            # subsets still get tested in the scenario audit, with sampled pixels.
            if name == 'seven-pcs-sixteen-isolated-monitors': continue
            for mask, state in availability_states(stations):
                with self.subTest(layout=name, mask=mask): assert_plan(self, state)

    def test_all_ineligible_causes_are_excluded_even_with_fresh_geometry(self):
        base = user_layout(True, True)
        causes = [dict(power=p) for p in ('sleep', 'hibernate', 'shutdown', 'off')]
        causes += [dict(fresh=False), dict(ready=False), dict(reachable=False), dict(enabled=False), dict(session=0)]
        expected = self.plan_factory(tuple(replace(s, power='off') if s.slot == 2 else s for s in base)).portals
        for cause in causes:
            state = tuple(replace(s, **cause) if s.slot == 2 else s for s in base)
            plan = assert_plan(self, state)
            self.assertEqual(plan.portals, expected)

    def test_unknown_power_requires_a_fresh_ready_helper(self):
        stations = (Station(1, (Rect(0, 0, 100, 100),)), Station(2, (Rect(100, 0, 100, 100),), power='unknown'))
        self.assertTrue(self.plan_factory(stations).portals)
        self.assertFalse(self.plan_factory((stations[0], replace(stations[1], fresh=False))).portals)

    def test_offline_hub_does_not_strand_the_upper_or_left_neighbors(self):
        stations = tuple(replace(s, power='hibernate') if s.slot == 2 else s for s in user_layout(True, True))
        plan = assert_plan(self, stations)
        self.assertEqual({n.slot for n in plan.nodes}, {1, 3, 4, 5})
        self.assertTrue(any(p.virtual for p in plan.portals))
        self.assertEqual({p.target[0] for p in plan.portals if p.source[0] == 3}, {1, 4, 5})

    def test_a_chain_of_unavailable_pcs_connects_its_awake_border_neighbors(self):
        stations = tuple(Station(i + 1, (Rect(i * 100, 0, 100, 100),),
                                 power='sleep' if i in (1, 2, 3) else 'awake') for i in range(5))
        plan = assert_plan(self, stations)
        self.assertEqual({(p.source[0], p.target[0]) for p in plan.portals}, {(1, 5), (5, 1)})

    def test_only_upper_and_right_neighbors_awake_need_a_virtual_connection(self):
        stations = tuple(replace(s, power='sleep') if s.slot not in (3, 5) else s for s in user_layout(True, True))
        right = next(s.screens[0] for s in stations if s.slot == 3)
        upper = next(s.screens[0] for s in stations if s.slot == 5)
        # These two screens meet only at a corner: there is no positive-height
        # horizontal corridor or positive-width vertical corridor between them.
        self.assertEqual(upper.right, right.x)
        self.assertEqual(upper.bottom, right.y)
        plan = assert_plan(self, stations)
        self.assertEqual(len(plan.portals), 2)
        self.assertTrue(all(p.virtual for p in plan.portals))

    def test_no_nearest_neighbor_starvation(self):
        # Six destinations all lie to the right of a very tall source. A
        # nearest-PC-only rule would hide five of them behind the nearest one.
        stations = (Station(1, (Rect(0, 0, 100, 1200),)),) + tuple(
            Station(i + 2, (Rect(100, i * 200, 100, 200),)) for i in range(6))
        plan = assert_plan(self, stations)
        self.assertEqual({p.target[0] for p in plan.portals if p.source[0] == 1}, set(range(2, 8)))

    def test_disconnected_monitors_are_independently_reachable(self):
        stations = dict(named_layouts())['disjoint-monitors']
        plan = assert_plan(self, stations)
        self.assertEqual(len(plan.nodes), 4)
        self.assertEqual(len(components(stations[0])), 2)

    def test_inside_corner_preserves_both_directions_and_straight_landings(self):
        stations = (Station(1, (Rect(554, 192, 1366, 768),)),
                    Station(2, (Rect(0, 960, 1920, 1200), Rect(1920, 0, 3840, 2160))))
        plan = assert_plan(self, stations)
        self.assertEqual({p.edges[0].direction for p in plan.portals if p.source[0] == 1}, {RIGHT, DOWN})
        self.assertEqual(plan.cross(1, RIGHT, 1919, 492).landing, (1922, 492))
        self.assertEqual(plan.cross(1, DOWN, 1054, 959).landing, (1054, 962))

    def test_active_contacts_do_not_spread_to_other_monitor_edges(self):
        plan = self.plan_factory(user_layout())
        # Only the laptop's top edge touches PC 1. The large screen's top does not.
        self.assertEqual(plan.cross(2, UP, 170, 120).target, (1, 0))
        for x in (248, 400, 631):
            self.assertIsNone(plan.cross(2, UP, x, 0))
        # Nor should the non-overlapping parts of the laptop's top edge switch.
        for x in (8, 100, 230, 231):
            self.assertIsNone(plan.cross(2, UP, x, 120))

    def test_active_neighbours_keep_their_full_physical_contact_when_a_pc_sleeps(self):
        stations = (Station(1, (Rect(0, 0, 200, 200),)),
                    Station(2, (Rect(200, 0, 200, 100),)),
                    Station(3, (Rect(200, 100, 200, 100),)),
                    Station(4, (Rect(400, 0, 200, 200),), power='sleep'),
                    Station(5, (Rect(600, 0, 200, 200),)))
        plan = assert_plan(self, stations)
        for y in (8, 50, 99): self.assertEqual(plan.cross(1, RIGHT, 199, y).target, (2, 0))
        for y in (100, 150, 191): self.assertEqual(plan.cross(1, RIGHT, 199, y).target, (3, 0))

    def test_internal_seams_and_monitor_holes_are_not_landing_points(self):
        assert_plan(self, dict(named_layouts())['monitor-ring'])

    def test_input_order_translation_and_scaling_do_not_change_topology(self):
        stations = user_layout(True, True)
        original = self.plan_factory(stations)
        self.assertEqual(original.portals, self.plan_factory(tuple(reversed(stations))).portals)
        translated = tuple(replace(s, screens=tuple(Rect(r.x - 10000, r.y + 777, r.width, r.height) for r in reversed(s.screens))) for s in stations)
        transformed = assert_plan(self, translated)
        self.assertEqual([(p.source, p.target) for p in original.portals], [(p.source, p.target) for p in transformed.portals])
        scaled = tuple(replace(s, screens=tuple(Rect(r.x * 2, r.y * 2, r.width * 2, r.height * 2) for r in s.screens)) for s in stations)
        self.assertEqual([(p.source, p.target) for p in original.portals], [(p.source, p.target) for p in assert_plan(self, scaled).portals])

    def test_queued_handoff_is_rejected_after_power_or_geometry_change(self):
        stations = user_layout(True, True)
        plan = self.plan_factory(stations)
        portal = plan.portals[0]
        edge = portal.edges[0]
        intent = plan.cross(portal.source[0], edge.direction, *edge.point(edge.start))
        for change in (dict(power='sleep'), dict(fresh=False), dict(ready=False), dict(reachable=False),
                       dict(session=2), dict(screens=(Rect(-500, -500, 100, 100),))):
            state = tuple(replace(s, **change) if s.slot == intent.target[0] else s for s in stations)
            self.assertFalse(self.plan_factory(state).accepts(intent))
        self.assertTrue(self.plan_factory(stations).accepts(intent))

    def test_sleep_wake_cycle_does_not_resurrect_an_old_handoff(self):
        stations = user_layout(True, True)
        plan = self.plan_factory(stations, revision=10)
        portal = plan.portals[0]
        edge = portal.edges[0]
        intent = plan.cross(portal.source[0], edge.direction, *edge.point(edge.start))
        asleep = tuple(replace(s, power='sleep') if s.slot == intent.target[0] else s for s in stations)
        self.assertFalse(self.plan_factory(asleep, revision=11).accepts(intent))
        self.assertFalse(self.plan_factory(stations, revision=12).accepts(intent))

    def test_insufficient_edge_space_is_explicit_instead_of_false_connectivity(self):
        stations = (Station(1, (Rect(0, 0, 8, 8),)), Station(2, (Rect(8, 0, 8, 8),)))
        with self.assertRaises(UnsupportedLayout): self.plan_factory(stations, corner=8)
        assert_plan(self, stations, corner=0)

    def test_invalid_layouts_fail_before_planning(self):
        for stations in ((), tuple(Station(1, (Rect(0, 0, 100, 100),)) for _ in range(2)),
                         (Station(8, (Rect(0, 0, 100, 100),)),), (Station(1, ()),),
                         (Station(1, (Rect(0, 0, 0, 100),)),)):
            with self.assertRaises(UnsupportedLayout): self.plan_factory(stations)

    def test_seven_pcs_sixteen_disconnected_monitors_each(self):
        stations = dict(named_layouts())['seven-pcs-sixteen-isolated-monitors']
        plan = assert_plan(self, stations)
        self.assertEqual(len(plan.nodes), 112)


class OracleTests(unittest.TestCase):
    """Negative controls: a broken planner must not pass the reachability oracle."""
    def test_spreading_a_physical_contact_to_an_unconnected_monitor_is_detected(self):
        stations = user_layout()
        plan = build_plan(stations)
        broken = replace(plan, portals=tuple(replace(p, edges=p.edges + (Edge(UP, 0, 248, 632),))
                         if p.source[0] == 2 and p.target[0] == 1 else p for p in plan.portals))
        with self.assertRaises(AssertionError): validate_plan(self, stations, broken)
    def test_missing_route_and_disconnected_monitor_are_detected(self):
        stations = dict(named_layouts())['disjoint-monitors']
        plan = build_plan(stations)
        isolated = plan.nodes[-1].key
        broken = replace(plan, portals=tuple(p for p in plan.portals if p.source != isolated and p.target != isolated))
        # There can still be edges between both PC IDs; the monitor-component
        # check must detect the trap on the isolated local monitor.
        with self.assertRaises(AssertionError): validate_plan(self, stations, broken)

    def test_sleeping_destination_is_detected(self):
        stations = user_layout()
        plan = build_plan(stations)
        asleep = tuple(replace(s, power='sleep') if s.slot == 2 else s for s in stations)
        with self.assertRaises(AssertionError): validate_plan(self, asleep, plan)

    def test_hidden_destination_due_to_overlapping_bands_is_detected(self):
        stations = (Station(1, (Rect(0, 0, 100, 400),)),
                    Station(2, (Rect(100, 0, 100, 200),)), Station(3, (Rect(100, 200, 100, 200),)))
        plan = build_plan(stations)
        outgoing = [p for p in plan.portals if p.source[0] == 1]
        self.assertEqual(len(outgoing), 2)
        first, second = outgoing
        broken = replace(plan, portals=tuple(replace(p, edges=first.edges) if p == second else p for p in plan.portals))
        with self.assertRaises(AssertionError): validate_plan(self, stations, broken)

    def test_invalid_landing_is_detected(self):
        stations = user_layout()
        plan = build_plan(stations)
        original = Plan.cross
        def broken_cross(current, *args):
            result = original(current, *args)
            return replace(result, landing=(-99999, -99999)) if result else None
        with patch.object(Plan, 'cross', broken_cross), self.assertRaises(AssertionError):
            validate_plan(self, stations, plan)
