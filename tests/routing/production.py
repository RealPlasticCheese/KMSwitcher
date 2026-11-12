"""ctypes bridge to the compiled production router, not a Python reimplementation."""
import ctypes
from dataclasses import dataclass
from pathlib import Path
from diagnostics.routing.model import Component, Portal, Edge, Plan, Crossing, UnsupportedLayout

_loaded = None
_library = None
_router = None


class ProductionRouter:
    def __init__(self):
        global _library
        library = Path(__file__).resolve().parents[2] / 'build/desktop-tests/desktop-tests.dll'
        if _library is None: _library = ctypes.CDLL(str(library))
        self.native = _library
        self.native.kms_routing_probe_init.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int32),
                                                      ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        self.native.kms_routing_probe_cross.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_int32, ctypes.c_int32]
        self.native.kms_routing_probe_cross.restype = ctypes.c_uint

    def load(self, stations):
        global _loaded
        _loaded = None
        records = [v for s in stations for r in s.screens for v in (s.slot, r.x, r.y, r.width, r.height)]
        array = (ctypes.c_int32 * len(records))(*records)
        awake = sum(1 << (s.slot - 1) for s in stations if s.available)
        sleeping = sum(1 << (s.slot - 1) for s in stations if s.power in ('sleep', 'hibernate'))
        present = sum(1 << (s.slot - 1) for s in stations if s.reachable)
        if not self.native.kms_routing_probe_init(len(records) // 5, array, awake, sleeping, present):
            raise ValueError('Invalid production routing probe fixture')

    def cross(self, source, direction, point):
        return self.native.kms_routing_probe_cross(source, direction, *point)


def _prepare(stations, corner, revision):
    global _loaded, _router
    key = (revision, stations, corner)
    if _router is None: _router = ProductionRouter()
    native = _router.native
    native.kms_routing_probe_state.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int32),
                                              ctypes.POINTER(ctypes.c_int32), ctypes.c_uint, ctypes.c_uint32]
    native.kms_routing_probe_node.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int32)]
    native.kms_routing_probe_edges.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_int32)]
    native.kms_routing_probe_landing.argtypes = [ctypes.POINTER(ctypes.c_int32)]
    if _loaded != key:
        records = [v for s in stations for r in s.screens for v in (s.slot, r.x, r.y, r.width, r.height)]
        array = (ctypes.c_int32 * len(records))(*records)
        states = [0] * 49
        for s in stations:
            power = 0 if s.power == 'unknown' else 1 if s.power == 'awake' else 2 if s.power in ('sleep', 'hibernate') else 3
            states[7 * (s.slot - 1):7 * s.slot] = [power, s.fresh, s.ready, s.reachable, s.enabled, s.session, 10]
        state_array = (ctypes.c_int32 * len(states))(*states)
        result = native.kms_routing_probe_state(len(records) // 5, array, state_array, corner, revision)
        if result != 1: raise UnsupportedLayout('Production route planner rejected the layout/edge capacity')
        _loaded = key
    return native


@dataclass(frozen=True)
class ProductionPlan(Plan):
    stations: tuple
    corner: int
    revision: int

    def cross(self, slot, direction, x, y):
        native = _prepare(self.stations, self.corner, self.revision)
        target = native.kms_routing_probe_cross(slot, direction, x, y)
        if not target: return None
        point = (ctypes.c_int32 * 2)()
        native.kms_routing_probe_landing(point)
        landing = tuple(point)
        source = next((n.key for n in self.nodes if n.slot == slot and any(r.contains(x, y) for r in n.screens)), (slot, -1))
        destination = next((n.key for n in self.nodes if n.slot == target and any(r.contains(*landing) for r in n.screens)), (target, -1))
        return Crossing(self.snapshot, source, destination, landing)


def build_production_plan(stations, corner=8, revision=0):
    stations = tuple(sorted(stations, key=lambda s: s.slot))
    if not 1 <= len(stations) <= 7 or len({s.slot for s in stations}) != len(stations) or not 0 <= corner <= 64:
        raise UnsupportedLayout('Invalid production fixture')
    if any(not 1 <= s.slot <= 7 or not 1 <= len(s.screens) <= 16 for s in stations):
        raise UnsupportedLayout('Invalid production monitor fixture')
    native = _prepare(stations, corner, revision)
    nodes, indices = [], []
    by_slot = {s.slot: s for s in stations}
    for index in range(native.kms_routing_probe_nodes()):
        record = (ctypes.c_int32 * 4)()
        native.kms_routing_probe_node(index, record)
        slot, ordinal, mask, active = record
        if active:
            screens = tuple(sorted(r for i, r in enumerate(by_slot[slot].screens) if mask & (1 << i)))
            nodes.append(Component(slot, ordinal, screens)); indices.append(index)
    portals = []
    buffer = (ctypes.c_int32 * (4 * 16 * 17))()
    for source, a in zip(nodes, indices):
        for target, b in zip(nodes, indices):
            directions = native.kms_routing_probe_links(a, b)
            for direction in range(4):
                if not directions & (1 << direction): continue
                length = native.kms_routing_probe_edges(a, b, direction, buffer)
                if length:
                    edges = tuple(Edge(*buffer[4 * i:4 * i + 4]) for i in range(length))
                    portals.append(Portal(source.key, target.key, edges, bool(native.kms_routing_probe_virtual(a, b, direction))))
    # Match the reference's documented side/position ordering for comparisons.
    centers = {n.key: n.center for n in nodes}
    portals.sort(key=lambda p: (p.source, p.edges[0].direction,
                              centers[p.target][1 if p.edges[0].direction < 2 else 0], p.target))
    return ProductionPlan((revision, stations, corner), tuple(nodes), tuple(portals), stations, corner, revision)
