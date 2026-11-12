"""Reveal saved items in the desktop file manager, away from the GUI thread."""
import asyncio
from pathlib import Path
import subprocess
import sys


def reveal_path(path):
    path = Path(path).resolve(strict=True)
    if sys.platform == "win32":
        # Explorer expects its own /select syntax, including quotes around paths.
        # Windows file names cannot contain quotes; no command shell is involved.
        subprocess.Popen(f'explorer.exe /select,"{path}"', stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=subprocess.CREATE_NO_WINDOW)
        return
    try:
        asyncio.run(_show_items(path.as_uri()))
    except Exception:
        # Some Linux file managers do not implement selection through FileManager1.
        subprocess.run(["xdg-open", str(path.parent)], check=True, timeout=5,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


async def _show_items(uri):
    from dbus_next import Message, MessageType
    from dbus_next.aio import MessageBus
    bus = MessageBus()
    try:
        await asyncio.wait_for(bus.connect(), timeout=2)
        reply = await asyncio.wait_for(bus.call(Message(
            destination="org.freedesktop.FileManager1", path="/org/freedesktop/FileManager1",
            interface="org.freedesktop.FileManager1", member="ShowItems", signature="ass",
            body=[[uri], ""])), timeout=2)
        if reply.message_type == MessageType.ERROR:
            raise OSError("The file manager could not select the saved item")
    finally:
        bus.disconnect()
