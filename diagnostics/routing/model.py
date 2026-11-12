"""Independent routing specification: preserve contacts, connect groups, allocate edges.

This model provides differential checks for the compiled production router. Coordinates
are desktop pixels in one simulated coordinate space; all ranges are integer and
half open. Independent per-PC desk/desktop transforms are tested natively;
this reference uses a single simulated coordinate space.
"""
from dataclasses import dataclass
from itertools import combinations

LEFT, RIGHT, UP, DOWN = range(4)
OPPOSITE = (RIGHT, LEFT, DOWN, UP)


@dataclass(frozen=True, order=True)
class Rect:
    x: int
    y: int
    width: int
    height: int

    @property
    def right(self): return self.x + self.width
    @property
    def bottom(self): return self.y + self.height
    def contains(self, x, y): return self.x <= x < self.right and self.y <= y < self.bottom


@dataclass(frozen=True)
class Station:
    slot: int
    screens: tuple[Rect, ...]
    power: str = 'awake'
    fresh: bool = True
    ready: bool = True
    reachable: bool = True
    enabled: bool = True
    session: int = 1

    @property
    def available(self):
        return (self.power in ('awake', 'unknown') and self.fresh and self.ready
                and self.reachable and self.enabled and self.session > 0 and bool(self.screens))


@dataclass(frozen=True, order=True)
class Edge:
    direction: int
    fixed: int
    start: int
    end: int

    @property
    def length(self): return self.end - self.start
    def point(self, along): return (self.fixed, along) if self.direction < UP else (along, self.fixed)
    def includes(self, direction, x, y):
        fixed, along = (x, y) if direction < UP else (y, x)
        return self.direction == direction and fixed == self.fixed and self.start <= along < self.end


@dataclass(frozen=True)
class Component:
    slot: int
    index: int
    screens: tuple[Rect, ...]

    @property
    def key(self): return self.slot, self.index
    @property
    def center(self):
        return ((min(r.x for r in self.screens) + max(r.right for r in self.screens)) // 2,
                (min(r.y for r in self.screens) + max(r.bottom for r in self.screens)) // 2)


def touching(a, b):
    dx = min(a.right, b.right) - max(a.x, b.x)
    dy = min(a.bottom, b.bottom) - max(a.y, b.y)
    return dx >= 0 and dy >= 0 and (dx > 0 or dy > 0)


def components(station):
    pending = set(station.screens)
    groups = []
    while pending:
        group = {min(pending)}
        pending -= group
        while True:
            neighbors = {r for r in pending if any(touching(r, q) for q in group)}
            if not neighbors: break
            pending -= neighbors
            group |= neighbors
        groups.append(tuple(sorted(group)))
    return [Component(station.slot, i, group) for i, group in enumerate(sorted(groups))]


def exposed_edges(component, corner):
    """Subtract internal seams and overlaps; retain only points the real helper can cross."""
    raw = []
    for r in component.screens:
        for direction in range(4):
            fixed = (r.x, r.right - 1, r.y, r.bottom - 1)[direction]
            start, end = (r.y, r.bottom) if direction < UP else (r.x, r.right)
            intervals = [(start + corner, end - corner)] if end - start > 2 * corner else []
            outside = fixed + (-1 if direction in (LEFT, UP) else 1)
            for q in component.screens:
                inside = q.x <= outside < q.right if direction < UP else q.y <= outside < q.bottom
                if not inside: continue
                lo, hi = (q.y, q.bottom) if direction < UP else (q.x, q.right)
                intervals = [(a, min(b, lo)) for a, b in intervals if a < min(b, lo)] + [
                    (max(a, hi), b) for a, b in intervals if max(a, hi) < b]
            raw += [Edge(direction, fixed, a, b) for a, b in intervals]
    merged = []
    for edge in sorted(raw):
        if (merged and (merged[-1].direction, merged[-1].fixed) == (edge.direction, edge.fixed)
                and edge.start <= merged[-1].end):
            previous = merged.pop()
            edge = Edge(edge.direction, edge.fixed, previous.start, max(previous.end, edge.end))
        merged.append(edge)
    return tuple(merged)


def contacts(a, b):
    directions = set()
    for r in a.screens:
        for s in b.screens:
            if min(r.bottom, s.bottom) > max(r.y, s.y):
                if r.right == s.x: directions.add(RIGHT)
                if s.right == r.x: directions.add(LEFT)
            if min(r.right, s.right) > max(r.x, s.x):
                if r.bottom == s.y: directions.add(DOWN)
                if s.bottom == r.y: directions.add(UP)
    return tuple(sorted(directions))


def contact(a, b):
    return next(iter(contacts(a, b)), None)


def distance(a, b):
    return min(max(r.x - s.right, s.x - r.right, 0) ** 2 +
               max(r.y - s.bottom, s.y - r.bottom, 0) ** 2
               for r in a.screens for s in b.screens)


def direction_to(a, b):
    dx, dy = b.center[0] - a.center[0], b.center[1] - a.center[1]
    if abs(dx) >= abs(dy): return RIGHT if dx >= 0 else LEFT
    return DOWN if dy >= 0 else UP


def bypass_pairs(stations, active):
    """Join awake neighbors bordering the same connected unavailable region."""
    saved = tuple(c for s in stations for c in components(s))
    active_keys = {c.key for c in active}
    inactive = {c.key for c in saved} - active_keys
    adjacency = {c.key: set() for c in saved}
    for a, b in combinations(saved, 2):
        if a.slot != b.slot and contact(a, b) is not None:
            adjacency[a.key].add(b.key)
            adjacency[b.key].add(a.key)
    pairs = set()
    while inactive:
        pending, boundary = [min(inactive)], set()
        inactive.remove(pending[0])
        while pending:
            key = pending.pop()
            boundary |= adjacency[key] & active_keys
            neighbors = adjacency[key] & inactive
            inactive -= neighbors
            pending += sorted(neighbors)
        pairs |= {(a, b) for a, b in combinations(sorted(boundary), 2) if a[0] != b[0]}
    return pairs


class UnsupportedLayout(ValueError):
    pass


@dataclass(frozen=True)
class Portal:
    source: tuple[int, int]
    target: tuple[int, int]
    edges: tuple[Edge, ...]
    virtual: bool

    @property
    def length(self): return sum(e.length for e in self.edges)

    def point_at(self, offset):
        for edge in self.edges:
            if offset < edge.length: return edge.point(edge.start + offset)
            offset -= edge.length
        raise ValueError('Portal offset outside its edges')

    def offset(self, direction, x, y):
        offset = 0
        for edge in self.edges:
            if edge.includes(direction, x, y):
                return offset + ((y if direction < UP else x) - edge.start)
            offset += edge.length
        return None


@dataclass(frozen=True)
class Crossing:
    snapshot: tuple
    source: tuple[int, int]
    target: tuple[int, int]
    landing: tuple[int, int]


@dataclass(frozen=True)
class Plan:
    snapshot: tuple
    nodes: tuple[Component, ...]
    portals: tuple[Portal, ...]

    def cross(self, slot, direction, x, y):
        for portal in self.portals:
            if portal.source[0] != slot: continue
            offset = portal.offset(direction, x, y)
            if offset is None: continue
            destination = next(n for n in self.nodes if n.key == portal.target)
            for r in destination.screens:
                aligned = ((direction == RIGHT and r.x == x + 1 and r.y <= y < r.bottom) or
                           (direction == LEFT and r.right == x and r.y <= y < r.bottom) or
                           (direction == DOWN and r.y == y + 1 and r.x <= x < r.right) or
                           (direction == UP and r.bottom == y and r.x <= x < r.right))
                if aligned:
                    lx = r.x + 2 if direction == RIGHT else r.right - 3 if direction == LEFT else x
                    ly = r.y + 2 if direction == DOWN else r.bottom - 3 if direction == UP else y
                    return Crossing(self.snapshot, portal.source, portal.target,
                                    (max(r.x + 2, min(lx, r.right - 3)), max(r.y + 2, min(ly, r.bottom - 3))))
            reverse = next(p for p in self.portals if p.source == portal.target and p.target == portal.source
                           and p.edges[0].direction == (portal.edges[0].direction ^ 1))
            mapped = min(reverse.length - 1, offset * reverse.length // portal.length)
            lx, ly = reverse.point_at(mapped)
            side = reverse.edges[0].direction
            lx += 2 * ((side == LEFT) - (side == RIGHT))
            ly += 2 * ((side == UP) - (side == DOWN))
            return Crossing(self.snapshot, portal.source, portal.target, (lx, ly))
        return None

    def accepts(self, crossing):
        return crossing.snapshot == self.snapshot and any(n.key == crossing.target for n in self.nodes)


def _ahead(node, direction, x, y):
    plane, along = (x + (direction == RIGHT), y) if direction < UP else (y + (direction == DOWN), x)
    distances = []
    for r in node.screens:
        lo, hi = (r.x, r.right) if direction < UP else (r.y, r.bottom)
        start, end = (r.y, r.bottom) if direction < UP else (r.x, r.right)
        if not start <= along < end: continue
        if direction in (RIGHT, DOWN) and hi > plane: distances.append(max(0, lo - plane))
        if direction in (LEFT, UP) and lo < plane: distances.append(max(0, plane - hi))
    return min(distances, default=None)


def _eligible(source, target, direction, virtual, x, y, nodes):
    if not virtual:
        if any(r.contains(x, y) for r in target.screens): return False
        return any((direction == RIGHT and r.x == x + 1 and r.y <= y < r.bottom) or
                   (direction == LEFT and r.right == x and r.y <= y < r.bottom) or
                   (direction == DOWN and r.y == y + 1 and r.x <= x < r.right) or
                   (direction == UP and r.bottom == y and r.x <= x < r.right)
                   for r in target.screens)
    distance = _ahead(target, direction, x, y)
    # An aligned awake screen wins over a farther destination. Diagonal temporary
    # links use rays with no intervening awake PC. Source/destination monitors
    # belong to the same switch endpoints. Equal-distance overlays are ties.
    for node in nodes:
        if node.slot in (source.slot, target.slot): continue
        nearer = _ahead(node, direction, x, y)
        if nearer is not None and (distance is None or nearer < distance): return False
    return True


def _cells(edge, nodes):
    cuts = {edge.start, edge.end}
    for node in nodes:
        for r in node.screens:
            for value in (r.y, r.bottom) if edge.direction < UP else (r.x, r.right):
                if edge.start < value < edge.end: cuts.add(value)
    values = sorted(cuts)
    return tuple(Edge(edge.direction, edge.fixed, a, b) for a, b in zip(values, values[1:]))


def _allocate(source, requests, edges, nodes):
    pieces = {(target.key, direction): [] for target, direction, _ in requests}
    for direction in range(4):
        destinations = sorted((r for r in requests if r[1] == direction),
                              key=lambda r: (r[0].center[1 if direction < UP else 0], r[0].key))
        for edge in sorted((e for e in edges if e.direction == direction), key=lambda e: (e.start, e.fixed)):
            for cell in _cells(edge, nodes):
                x, y = cell.point(cell.start)
                physical = [r for r in destinations if not r[2] and _eligible(source, r[0], direction, False, x, y, nodes)]
                eligible = physical or [r for r in destinations if r[2] and _eligible(source, r[0], direction, True, x, y, nodes)]
                for i, (target, _, _) in enumerate(eligible):
                    lo, hi = i * cell.length // len(eligible), (i + 1) * cell.length // len(eligible)
                    if lo < hi: pieces[target.key, direction].append(Edge(direction, cell.fixed, cell.start + lo, cell.start + hi))
    result = []
    for target, direction, virtual in sorted(requests, key=lambda r: (r[1], r[0].center[1 if r[1] < UP else 0], r[0].key)):
        merged = []
        for edge in sorted(pieces[target.key, direction], key=lambda e: (e.fixed, e.start)):
            if merged and merged[-1].fixed == edge.fixed and merged[-1].end == edge.start:
                before = merged.pop(); edge = Edge(direction, edge.fixed, before.start, edge.end)
            merged.append(edge)
        if not merged: raise UnsupportedLayout(f'{source.key} -> {target.key}: no usable unblocked crossing pixels')
        result.append(Portal(source.key, target.key, tuple(sorted(merged, key=lambda e: (e.start, e.fixed))), virtual))
    return result


def _can_cross(source, target, side, virtual, nodes, edges, links=()):
    physical = [(b, d) if source == a else (a, d ^ 1) for a, b, d, temporary in links
                if not temporary and source in (a, b)]
    for edge in edges[source.key]:
        if edge.direction != side: continue
        for cell in _cells(edge, nodes):
            x, y = cell.point(cell.start)
            if virtual and any(d == side and _eligible(source, n, side, False, x, y, nodes) for n, d in physical): continue
            if _eligible(source, target, side, virtual, x, y, nodes): return True
    return False


def _virtual_direction(a, b, nodes, edges, links):
    preferred = direction_to(a, b)
    # Try the main axis, then the other axis, then opposite sides. A connection
    # is usable only when BOTH ends have an exposed, unobstructed crossing.
    for d in (preferred, preferred ^ 2, preferred ^ 3, preferred ^ 1):
        if _can_cross(a, b, d, True, nodes, edges, links) and _can_cross(b, a, d ^ 1, True, nodes, edges, links): return d
    return None


def build_plan(stations, corner=8, revision=0):
    stations = tuple(sorted(stations, key=lambda s: s.slot))
    if not 1 <= len(stations) <= 7 or len({s.slot for s in stations}) != len(stations):
        raise UnsupportedLayout('Expected one to seven unique PC slots')
    if not 0 <= corner <= 64:
        raise UnsupportedLayout('Corner exclusion must be 0..64 pixels')
    for station in stations:
        if not 1 <= station.slot <= 7 or not 1 <= len(station.screens) <= 16:
            raise UnsupportedLayout('Expected slots 1..7 with 1..16 remembered monitors')
        if any(r.width < 8 or r.height < 8 for r in station.screens):
            raise UnsupportedLayout('Monitor dimensions must be at least eight pixels')
    nodes = tuple(c for s in stations if s.available for c in components(s))
    # The caller advances revision after every availability/geometry transition,
    # including sleep -> awake with identical geometry and helper session.
    snapshot = (revision, stations, corner)
    if len({n.slot for n in nodes}) < 2:
        return Plan(snapshot, nodes, ())
    parent = {n.key: n.key for n in nodes}

    def find(key):
        while parent[key] != key:
            parent[key] = parent[parent[key]]
            key = parent[key]
        return key

    edges = {n.key: exposed_edges(n, corner) for n in nodes}
    bypass = bypass_pairs(stations, nodes)
    candidates = []
    for a, b in combinations(nodes, 2):
        if a.slot == b.slot: continue
        shared = tuple(d for d in contacts(a, b) if _can_cross(a, b, d, False, nodes, edges)
                       and _can_cross(b, a, d ^ 1, False, nodes, edges))
        kind = 0 if shared else 1 if (a.key, b.key) in bypass else 2
        candidates.append((kind, distance(a, b), a.key, b.key, a, b, shared))
    candidates.sort(key=lambda c: c[:4])
    links = []
    for kind, _, _, _, a, b, shared in candidates:
        if kind == 2 and find(a.key) == find(b.key): continue
        directions = shared if kind == 0 else (_virtual_direction(a, b, nodes, edges, links),)
        if directions == (None,): continue
        parent[find(a.key)] = find(b.key)
        for direction in directions:
            links.append((a, b, direction, kind != 0))
    if len({find(n.key) for n in nodes}) > 1:
        raise UnsupportedLayout('No unobstructed routes connect all monitor groups')
    portals = []
    for node in nodes:
        requests = [(b, direction, virtual) if node == a else (a, OPPOSITE[direction], virtual)
                    for a, b, direction, virtual in links if node in (a, b)]
        portals += _allocate(node, requests, edges[node.key], nodes)
    return Plan(snapshot, nodes, tuple(portals))
