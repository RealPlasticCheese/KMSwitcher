"""Qt station items with bounds and movement notifications for the desk editor."""
from itertools import combinations

from PySide6.QtCore import QLineF, QPointF, QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QPainter, QPainterPath, QPen, QPixmap
from PySide6.QtWidgets import (QGraphicsItem, QGraphicsItemGroup, QGraphicsRectItem, QGraphicsSimpleTextItem,
                               QGraphicsView, QGridLayout, QLabel, QWidget)


DESK_BACKGROUND = "#26282c"
EDGE_STYLES = {
    0: ("No crossing", "#ffffff", Qt.PenStyle.SolidLine),
    1: ("Exact crossing", "#ff5252", Qt.PenStyle.SolidLine),
    2: ("Within tolerance", "#ff5252", Qt.PenStyle.DotLine),
    3: ("Remembered crossing", "#76d8ff", Qt.PenStyle.DashLine),
    4: ("Fallback crossing", "#ffd56a", Qt.PenStyle.DashDotLine),
}


def edge_pens(kind):
    _, color, style = EDGE_STYLES[kind]
    pen = QPen(QColor(color), 5, style, Qt.PenCapStyle.FlatCap)
    pen.setCosmetic(True)
    if kind != 2:
        return (pen,)
    base = QPen(QColor(EDGE_STYLES[0][1]), 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.FlatCap)
    base.setCosmetic(True)
    return base, pen


def overlap_brush():
    return QBrush(QColor("#f6b94b"), Qt.BrushStyle.BDiagPattern)


def crossing_legend(parent):
    widget = QWidget(parent)
    widget.setObjectName("DeskCrossingKey")
    layout = QGridLayout(widget)
    layout.setContentsMargins(0, 0, 0, 0)
    for index in range(6):
        pixmap = QPixmap(66, 18)
        pixmap.fill(QColor(DESK_BACKGROUND))
        painter = QPainter(pixmap)
        if index in EDGE_STYLES:
            title = EDGE_STYLES[index][0]
            for pen in edge_pens(index):
                painter.setPen(pen)
                painter.drawLine(4, 9, 62, 9)
        else:
            title = "Screen overlap"
            painter.fillRect(4, 3, 58, 12, overlap_brush())
        painter.end()
        row, column = divmod(index, 3)
        swatch = QLabel()
        swatch.setPixmap(pixmap)
        layout.addWidget(swatch, row, 2 * column)
        layout.addWidget(QLabel(title), row, 2 * column + 1)
        layout.setColumnStretch(2 * column + 1, 1)
    return widget


def desk_separations(screens):
    """Report intersections and the visible gap between each pair of facing screens."""
    for (first, a), (second, b) in combinations(screens, 2):
        if first == second:
            continue
        area = a.intersected(b)
        if not area.isEmpty():
            yield "overlap", round(min(area.width(), area.height())), area.center(), area
            continue
        for horizontal in (True, False):
            left, right = (a, b) if (a.left() < b.left() if horizontal else a.top() < b.top()) else (b, a)
            low, high = (left.right(), right.left()) if horizontal else (left.bottom(), right.top())
            start = max(left.top(), right.top()) if horizontal else max(left.left(), right.left())
            end = min(left.bottom(), right.bottom()) if horizontal else min(left.right(), right.right())
            if low >= high or start >= end:
                continue
            spans = [(start, end)]
            for _, other in screens:
                if other is a or other is b:
                    continue
                near, far = (other.left(), other.right()) if horizontal else (other.top(), other.bottom())
                if near >= high or far <= low:
                    continue
                before, after = (other.top(), other.bottom()) if horizontal else (other.left(), other.right())
                spans = [(x, y) for lo, hi in spans for x, y in
                         ((lo, min(hi, before)), (max(lo, after), hi)) if x < y]
                if not spans:
                    break
            if spans:
                start, end = max(spans, key=lambda span: span[1] - span[0])
                across, along = (low + high) / 2, (start + end) / 2
                position = QPointF(across, along) if horizontal else QPointF(along, across)
                yield "gap", round(high - low), position, None


def desk_label(scene, position, label):
    item = QGraphicsRectItem()
    item.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIgnoresTransformations)
    item.setPen(QPen(Qt.PenStyle.NoPen))
    item.setBrush(QColor(DESK_BACKGROUND))
    item.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
    text = QGraphicsSimpleTextItem(label, item)
    text.setBrush(QColor("#ffffff"))
    text.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
    box = text.boundingRect()
    text.setPos(-box.width() / 2, -box.height() / 2)
    item.setRect(box.translated(text.pos()).adjusted(-4, -2, 4, 2))
    item.setPos(position)
    item.setZValue(4)
    scene.addItem(item)
    return item


def exposed_edges(screens):
    """Yield (side, coordinate, start, end) segments outside a monitor union."""
    for rect in screens:
        for side, coordinate, start, end in (
                ("left", rect.left(), rect.top(), rect.bottom()),
                ("right", rect.right(), rect.top(), rect.bottom()),
                ("top", rect.top(), rect.left(), rect.right()),
                ("bottom", rect.bottom(), rect.left(), rect.right())):
            segments = [(start, end)]
            for other in screens:
                if other is rect:
                    continue
                covered = {"left": other.left() < coordinate <= other.right(),
                           "right": other.left() <= coordinate < other.right(),
                           "top": other.top() < coordinate <= other.bottom(),
                           "bottom": other.top() <= coordinate < other.bottom()}[side]
                if not covered:
                    continue
                low, high = (other.top(), other.bottom()) if side in ("left", "right") else (other.left(), other.right())
                segments = [(a, b) for lo, hi in segments
                            for a, b in ((lo, min(hi, low)), (max(lo, high), hi)) if a < b]
            for start, end in segments:
                yield side, coordinate, start, end


def shared_edges(screens, neighbors):
    """Return visible contact lines; internal seams and corner-only contacts do not count."""
    opposites = {"left": "right", "right": "left", "top": "bottom", "bottom": "top"}
    others = list(exposed_edges(neighbors))
    for side, coordinate, start, end in exposed_edges(screens):
        for other_side, other_coordinate, low, high in others:
            if other_side != opposites[side] or abs(coordinate - other_coordinate) > .001:
                continue
            low, high = max(start, low), min(end, high)
            if low < high:
                yield (QLineF(coordinate, low, coordinate, high) if side in ("left", "right")
                       else QLineF(low, coordinate, high, coordinate))


def snap_position(screens, position, neighbors, tolerance):
    """Snap actual screens into exposed contacts, including two sides of an inside corner."""
    # Accept a single rectangle for callers arranging single-screen stations.
    if isinstance(screens, QRectF):
        screens = [screens]
    candidates = []
    xs, ys = set(), set()
    for screen in screens:
        proposed = screen.translated(position)
        for other in neighbors:
            if proposed.top() < other.bottom() + tolerance and proposed.bottom() > other.top() - tolerance:
                for x in (other.left() - screen.right(), other.right() - screen.left()):
                    if abs(x - position.x()) <= tolerance:
                        xs.add(x)
                        y = min((other.top() - screen.top(), other.bottom() - screen.bottom(),
                                 other.center().y() - screen.center().y()), key=lambda y: abs(y - position.y()))
                        candidates.append(QPointF(x, y if abs(y - position.y()) <= tolerance else position.y()))
            if proposed.left() < other.right() + tolerance and proposed.right() > other.left() - tolerance:
                for y in (other.top() - screen.bottom(), other.bottom() - screen.top()):
                    if abs(y - position.y()) <= tolerance:
                        ys.add(y)
                        x = min((other.left() - screen.left(), other.right() - screen.right(),
                                 other.center().x() - screen.center().x()), key=lambda x: abs(x - position.x()))
                        candidates.append(QPointF(x if abs(x - position.x()) <= tolerance else position.x(), y))
    candidates.extend(QPointF(x, y) for x in sorted(xs) for y in sorted(ys))
    def rank(candidate):
        contacts = list(shared_edges([r.translated(candidate) for r in screens], neighbors))
        axes = len({line.dx() == 0 for line in contacts})
        return -axes, (candidate.x() - position.x()) ** 2 + (candidate.y() - position.y()) ** 2
    candidates.sort(key=rank)
    for candidate in candidates:
        placed = [r.translated(candidate) for r in screens]
        if (not any(r.intersects(other) for r in placed for other in neighbors)
                and any(shared_edges(placed, neighbors))):
            return candidate
    return position


class ScreenItem(QGraphicsRectItem):
    def __init__(self, rect, label):
        super().__init__(*rect)
        self.label = label
        self.setToolTip(label)

    def paint(self, painter, option, widget=None):
        super().paint(painter, option, widget)
        # Keep text readable at any zoom, but inside its own monitor preview.
        bounds = painter.worldTransform().mapRect(self.rect()).adjusted(6, 6, -6, -6)
        painter.save()
        painter.resetTransform()
        painter.setClipRect(bounds, Qt.ClipOperation.IntersectClip)
        painter.setPen(Qt.GlobalColor.white)
        text = painter.fontMetrics().elidedText(self.label, Qt.TextElideMode.ElideRight, max(0, int(bounds.width())))
        painter.drawText(bounds, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop, text)
        painter.restore()


class StationItem(QGraphicsItemGroup):
    def __init__(self, width, height, moved, released, snap, pressed):
        super().__init__()
        self.area = QRectF(0, 0, width, height)
        self.screens = []
        self.moved = moved
        self.released = released
        self.snap = snap
        self.pressed = pressed
        self.setFlags(QGraphicsItem.GraphicsItemFlag.ItemIsMovable |
                      QGraphicsItem.GraphicsItemFlag.ItemIsSelectable |
                      QGraphicsItem.GraphicsItemFlag.ItemSendsGeometryChanges)

    def placed_screens(self):
        return [rect.translated(self.pos()) for rect in self.screens]

    def shape(self):
        # QGraphicsItemGroup's default shape fills the enclosing rectangle,
        # including empty space that may contain a different dongle's screen.
        path = QPainterPath()
        path.setFillRule(Qt.FillRule.WindingFill)
        for rect in self.screens:
            path.addRect(rect)
        return path

    def itemChange(self, change, value):
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionChange:
            if self.scene() and self.scene().mouseGrabberItem():
                value = self.snap(value)
            return QPointF(max(-100000, min(100000, round(value.x()))),
                           max(-100000, min(100000, round(value.y()))))
        result = super().itemChange(change, value)
        if change == QGraphicsItem.GraphicsItemChange.ItemPositionHasChanged:
            self.moved(self)
        return result

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.pressed()
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        super().mouseReleaseEvent(event)
        self.released()


class LayoutView(QGraphicsView):
    def fit_layout(self):
        if not self.scene().items():
            return
        bounds = self.scene().itemsBoundingRect()
        margin = max(80, max(bounds.width(), bounds.height()) * .1)
        self.setSceneRect(bounds.adjusted(-margin, -margin, margin, margin))
        self.fitInView(self.sceneRect(), Qt.AspectRatioMode.KeepAspectRatio)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if not self.scene().mouseGrabberItem():
            self.fit_layout()
