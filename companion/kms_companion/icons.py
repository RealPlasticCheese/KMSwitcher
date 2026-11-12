"""Application artwork rendered at native tray sizes without font dependencies."""


def application_icon():
    from PySide6.QtCore import Qt, QRectF
    from PySide6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap

    icon = QIcon()
    for size in (16, 20, 24, 32, 40, 48, 64, 128, 256):
        pixmap = QPixmap(size, size)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.scale(size / 16, size / 16)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#38bdf8"))
        painter.drawRoundedRect(QRectF(1, 1, 14, 11), 1, 1)
        painter.drawRect(QRectF(7, 12, 2, 2))
        painter.drawRoundedRect(QRectF(4, 14, 8, 1), .5, .5)
        painter.setBrush(QColor("#0c4a6e"))
        painter.drawRect(QRectF(2, 2, 12, 9))

        # Outline the letters so every platform renders the same compact KM mark.
        letters = QPainterPath()
        letters.moveTo(3.5, 3.5)
        letters.lineTo(3.5, 9.5)
        letters.moveTo(5.5, 3.5)
        letters.lineTo(3.5, 6.5)
        letters.lineTo(5.5, 9.5)
        letters.moveTo(7.5, 9.5)
        letters.lineTo(7.5, 3.5)
        letters.lineTo(10, 7)
        letters.lineTo(12.5, 3.5)
        letters.lineTo(12.5, 9.5)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.setPen(QPen(QColor("#ffffff"), 1, Qt.PenStyle.SolidLine,
                            Qt.PenCapStyle.SquareCap, Qt.PenJoinStyle.MiterJoin))
        painter.drawPath(letters)
        painter.end()
        icon.addPixmap(pixmap)
    return icon
