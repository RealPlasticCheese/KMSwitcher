"""Monitor union boundaries; station placement never assumes a monitor count."""
from dataclasses import dataclass


POINTER_PARKING_MODES = ("bottom-right", "bottom-left", "top-right", "top-left",
                         "top-centre", "bottom-centre", "left-centre", "right-centre", "disabled")


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    width: int
    height: int

    def contains(self, x, y):
        return self.x <= x < self.x + self.width and self.y <= y < self.y + self.height

    def as_dict(self):
        return vars(self)


def normalize(screens):
    if not screens:
        raise ValueError("No active monitors")
    ox, oy = min(r.x for r in screens), min(r.y for r in screens)
    return [Rect(r.x - ox, r.y - oy, r.width, r.height) for r in screens], (ox, oy)


def parking_position(screens, mode):
    """Choose a point on the outermost real screen edge, never a monitor gap."""
    if mode not in POINTER_PARKING_MODES:
        raise ValueError("Unknown pointer parking mode")
    if mode == "disabled" or not screens:
        return None
    edge, alignment = mode.split("-")
    if edge in ("top", "bottom"):
        boundary = min(r.y for r in screens) if edge == "top" else max(r.y + r.height - 1 for r in screens)
        candidates = [r for r in screens if (r.y if edge == "top" else r.y + r.height - 1) == boundary]
        if alignment == "left":
            return min(r.x for r in candidates), boundary
        if alignment == "right":
            return max(r.x + r.width - 1 for r in candidates), boundary
        centre = (min(r.x for r in candidates) + max(r.x + r.width - 1 for r in candidates)) // 2
        points = [(min(max(centre, r.x), r.x + r.width - 1), boundary) for r in candidates]
        return min(points, key=lambda p: (abs(p[0] - centre), p))
    boundary = min(r.x for r in screens) if edge == "left" else max(r.x + r.width - 1 for r in screens)
    candidates = [r for r in screens if (r.x if edge == "left" else r.x + r.width - 1) == boundary]
    centre = (min(r.y for r in candidates) + max(r.y + r.height - 1 for r in candidates)) // 2
    points = [(boundary, min(max(centre, r.y), r.y + r.height - 1)) for r in candidates]
    return min(points, key=lambda p: (abs(p[1] - centre), p))


def crossing(screens, point, motion, corner=8):
    """Return exposed edge being pushed; internal monitor seams never switch."""
    x, y = point
    dx, dy = motion
    directions = sorted(((0, -1, 0, -dx), (1, 1, 0, dx),
                         (2, 0, -1, -dy), (3, 0, 1, dy)), key=lambda d: d[3], reverse=True)
    for direction, sx, sy, pressure in directions:
        if pressure <= 0 or any(r.contains(x + sx, y + sy) for r in screens):
            continue
        for r in screens:
            if not r.contains(x, y):
                continue
            along = y - r.y if sx else x - r.x
            length = r.height if sx else r.width
            if corner <= along < length - corner:
                return direction
    return None
