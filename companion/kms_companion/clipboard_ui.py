"""Qt clipboard access always runs on the GUI thread; transfers never open a window."""
import json
from pathlib import Path
import secrets
import shutil
import struct

from .clipboard_preferences import content_type


RECEIVED_FORMAT = "application/x-kmswitcher-received"


def capture_clipboard(clipboard, directory, limit, *, automatic_types=None):
    from PySide6.QtCore import QBuffer, QIODevice, QUrl
    from PySide6.QtGui import QImage
    mime = clipboard.mimeData()
    if automatic_types is not None and (mime is None or mime.hasFormat(RECEIVED_FORMAT)):
        return None
    if mime is None:
        raise ValueError("The clipboard is empty")
    formats, files = {}, []
    urls = list(mime.urls()) if mime.hasUrls() else []
    if not urls and mime.hasFormat("x-special/gnome-copied-files"):
        lines = bytes(mime.data("x-special/gnome-copied-files")).decode("utf-8").splitlines()
        if not lines or lines[0] not in ("copy", "cut"):
            raise ValueError("Invalid file-manager clipboard")
        urls = [QUrl(line) for line in lines[1:] if line]
    if urls and any(url.isLocalFile() for url in urls):
        if not all(url.isLocalFile() for url in urls):
            raise ValueError("Copy local files without mixing remote URLs")
        files = [url.toLocalFile() for url in urls]
    else:
        if mime.hasText():
            formats["text/plain"] = mime.text().encode("utf-8")
        if mime.hasHtml():
            formats["text/html"] = mime.html().encode("utf-8")
        for name in ("text/rtf", "application/rtf"):
            if mime.hasFormat(name):
                formats[name] = bytes(mime.data(name))
        native_rtf = 'application/x-qt-windows-mime;value="Rich Text Format"'
        if mime.hasFormat(native_rtf) and "text/rtf" not in formats:
            formats["text/rtf"] = bytes(mime.data(native_rtf))
    native_image = not files and mime.hasImage()
    png = not files and (native_image or mime.hasFormat("image/png"))
    format_names = formats.keys() | {"image/png"} if png else formats.keys()
    if automatic_types is not None and content_type(dict(roots=files, formats=format_names)) not in automatic_types:
        return None
    if native_image:
        image = QImage(mime.imageData())
        if image.isNull() or image.sizeInBytes() > limit:
            raise ValueError("Image is invalid or exceeds this computer's clipboard size limit")
        buffer = QBuffer(); buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        if not image.save(buffer, "PNG"):
            raise ValueError("Cannot encode the clipboard image")
        formats["image/png"] = bytes(buffer.data())
    elif png:
        formats["image/png"] = bytes(mime.data("image/png"))
    if not formats and not files:
        raise ValueError("Copy text, formatted text, an image, or local files/folders first")
    if sum(len(data) for data in formats.values()) > limit:
        raise ValueError("Clipboard exceeds this computer's size limit")
    capture = Path(directory) / "clipboard" / "outgoing" / secrets.token_hex(16)
    capture.mkdir(parents=True)
    entries = {}
    descriptor = capture / "capture.json"
    try:
        for index, (name, data) in enumerate(formats.items()):
            entry = f"format-{index}"
            (capture / entry).write_bytes(data)
            entries[name] = entry
        descriptor.write_text(json.dumps({"formats": entries, "files": files}), encoding="utf-8")
    except OSError:
        shutil.rmtree(capture, ignore_errors=True)
        raise
    return str(descriptor)


def publish_clipboard(clipboard, content):
    from PySide6.QtCore import QMimeData, QUrl
    from PySide6.QtGui import QImage
    directory, manifest = Path(content["directory"]), content["manifest"]
    mime = QMimeData()
    mime.setData(RECEIVED_FORMAT, b"1")
    if manifest["roots"]:
        urls = [QUrl.fromLocalFile(str(directory / root)) for root in manifest["roots"]]
        mime.setUrls(urls)
        mime.setData("x-special/gnome-copied-files", ("copy\n" + "\n".join(u.toString() for u in urls)).encode())
        mime.setData("application/x-kde-cutselection", b"0")
        mime.setData('application/x-qt-windows-mime;value="Preferred DropEffect"', struct.pack("<I", 1))
    else:
        for name, entry in manifest["formats"].items():
            data = (directory / entry).read_bytes()
            if name == "text/plain":
                mime.setText(data.decode("utf-8"))
            elif name == "text/html":
                mime.setHtml(data.decode("utf-8"))
            elif name == "image/png":
                if (len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR"):
                    raise ValueError("Received image is not PNG")
                width, height = struct.unpack_from(">II", data, 16)
                if not width or not height or width * height * 8 > content.get("limit", 256 * 1024 * 1024):
                    raise ValueError("Received image exceeds the clipboard pixel limit")
                image = QImage.fromData(data, "PNG")
                if image.isNull():
                    raise ValueError("Received image cannot be decoded")
                mime.setImageData(image)
                mime.setData(name, data)
            else:
                mime.setData(name, data)
                if name in ("text/rtf", "application/rtf"):
                    mime.setData('application/x-qt-windows-mime;value="Rich Text Format"', data)
    clipboard.setMimeData(mime)
