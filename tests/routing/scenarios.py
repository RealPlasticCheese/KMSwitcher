"""Deterministic layouts and state enumeration; no running desktop or hardware."""
from dataclasses import replace
import random
from diagnostics.routing.model import Rect, Station


def user_layout(extra_left=False, extra_above=False):
    # Synthetic corner geometry. Slot 2 has an inside corner.
    stations = [Station(1, (Rect(120, 0, 100, 120),)),
                Station(2, (Rect(0, 120, 240, 240), Rect(240, 0, 400, 360))),
                Station(3, (Rect(640, 0, 400, 360),))]
    if extra_left: stations.append(Station(4, (Rect(-240, 120, 240, 240),)))
    if extra_above: stations.append(Station(5, (Rect(240, -240, 400, 240),)))
    return tuple(stations)


def named_layouts():
    yield 'user-three-pcs', user_layout()
    yield 'user-extra-left', user_layout(True)
    yield 'user-extra-left-and-above', user_layout(True, True)
    for n in range(1, 8):
        yield f'row-{n}', tuple(Station(i + 1, (Rect(i * 160, 0, 160, 120),)) for i in range(n))
        yield f'column-{n}', tuple(Station(i + 1, (Rect(0, i * 120, 160, 120),)) for i in range(n))
        yield f'diagonal-{n}', tuple(Station(i + 1, (Rect(i * 200, i * 170, 160, 120),)) for i in range(n))
        yield f'overlaid-{n}', tuple(Station(i + 1, (Rect(0, 0, 160, 120),)) for i in range(n))
    yield 'hub-with-six-neighbors', tuple(Station(i + 1, (r,)) for i, r in enumerate((
        Rect(200, 200, 400, 400), Rect(0, 200, 200, 200), Rect(0, 400, 200, 200),
        Rect(600, 200, 200, 200), Rect(600, 400, 200, 200),
        Rect(200, 0, 400, 200), Rect(200, 600, 400, 200))))
    yield 'disjoint-monitors', (Station(1, (Rect(0, 0, 160, 120), Rect(0, 500, 160, 120))),
                               Station(2, (Rect(500, 0, 160, 120), Rect(500, 500, 160, 120))))
    yield 'negative-origins', tuple(Station(i + 1, (Rect(i * 160 - 600, -500, 160, 120),)) for i in range(7))
    # A same-PC ring: inner boundaries must not be mistaken for outer/internal seams.
    yield 'monitor-ring', (Station(1, (Rect(0, 0, 300, 100), Rect(0, 100, 100, 100),
                                      Rect(200, 100, 100, 100), Rect(0, 200, 300, 100))),
                           Station(2, (Rect(100, 100, 100, 100),)), Station(3, (Rect(300, 0, 160, 300),)))
    yield 'seven-pcs-sixteen-monitors', tuple(Station(i + 1, tuple(
        Rect(i * 800 + (j % 4) * 160, (j // 4) * 120, 160, 120) for j in range(16))) for i in range(7))
    yield 'seven-pcs-sixteen-isolated-monitors', tuple(Station(i + 1, tuple(
        Rect(i * 800 + (j % 4) * 180, (j // 4) * 140, 160, 120) for j in range(16))) for i in range(7))


def generated_layouts(seed=72519, per_count=80):
    rng = random.Random(seed)
    for count in range(1, 8):
        for index in range(per_count):
            # Mix snapped grids with sparse/offset placements. Deliberately include
            # multiple monitors, holes, overlaps and disconnected local components.
            stations = []
            for slot in range(1, count + 1):
                x, y = rng.randrange(-4, 5) * 320, rng.randrange(-4, 5) * 240
                w, h = rng.choice((160, 240, 320)), rng.choice((120, 180, 240))
                shape = rng.randrange(6)
                screens = [Rect(x, y, w, h)]
                if shape in (1, 2, 3): screens.append(Rect(x + w, y + (h // 2 if shape == 2 else 0), w, h))
                if shape == 3: screens.append(Rect(x, y - h, w, h))
                if shape == 4: screens.append(Rect(x - w // 2, y + h // 2, w, h))
                if shape == 5: screens.append(Rect(x + w + 120, y + h + 80, w, h))
                stations.append(Station(slot, tuple(screens)))
            yield f'random-{count}-{index}', tuple(stations)


def availability_states(stations):
    for mask in range(1 << len(stations)):
        # Enumerate ALL active subsets, including zero/one awake PC. Different
        # exclusion causes are checked separately; they must give the same routes.
        yield mask, tuple(replace(s, power='awake' if mask & (1 << i) else 'sleep') for i, s in enumerate(stations))
