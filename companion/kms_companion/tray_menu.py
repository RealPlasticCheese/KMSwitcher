"""Tray actions require a left click or keyboard selection."""
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QMenu


class TrayMenu(QMenu):
    def __init__(self, parent):
        super().__init__(parent)
        self._pressed_action = None

    def mousePressEvent(self, event):
        point = event.position().toPoint()
        self._pressed_action = None
        if event.button() == Qt.MouseButton.LeftButton:
            self._pressed_action = self.actionAt(point)
        elif self.rect().contains(point):
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        pressed = self._pressed_action
        self._pressed_action = None
        # Qt can activate a highlighted action on a release from the gesture
        # that opened the popup. Require a click started on this same action.
        if (event.button() != Qt.MouseButton.LeftButton or pressed is None
                or pressed is not self.actionAt(event.position().toPoint())):
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def hideEvent(self, event):
        self._pressed_action = None
        super().hideEvent(event)
