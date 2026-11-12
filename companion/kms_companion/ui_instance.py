"""Exclusive settings/tray ownership and activation of an existing window."""
import hashlib
import time

from PySide6.QtCore import QLockFile
from PySide6.QtNetwork import QLocalServer, QLocalSocket

from .agent import config_directory


def server_name():
    return "kmswitcher-ui-" + hashlib.sha256(str(config_directory()).encode()).hexdigest()[:16]


def send_command(command):
    """Return whether a running UI received the command (including older releases)."""
    socket = QLocalSocket()
    socket.connectToServer(server_name())
    if not socket.waitForConnected(250):
        return False
    socket.write(command)
    sent = socket.waitForBytesWritten(250)
    socket.disconnectFromServer()
    return sent


class UiInstance:
    def __init__(self):
        directory = config_directory()
        directory.mkdir(parents=True, exist_ok=True)
        self.lock = QLockFile(str(directory / "ui.lock"))
        self.lock.setStaleLockTime(0)
        self.server = QLocalServer()

    def acquire(self, command):
        if send_command(command):
            return False
        deadline = time.monotonic() + 5
        while not self.lock.tryLock(0):
            if self.lock.error() != QLockFile.LockError.LockFailedError:
                raise RuntimeError("Cannot lock the KMSwitcher settings window")
            if send_command(command):
                return False
            if time.monotonic() >= deadline:
                raise RuntimeError("KMSwitcher is already starting or shutting down; try again shortly")
            time.sleep(.05)
        # Remove a crashed Unix socket only after acquiring exclusive ownership.
        QLocalServer.removeServer(server_name())
        if not self.server.listen(server_name()):
            self.lock.unlock()
            raise RuntimeError("Cannot create the settings-window connection")
        return True

    def receive(self, callback):
        def connected():
            while self.server.hasPendingConnections():
                socket = self.server.nextPendingConnection()

                def ready(socket=socket):
                    message = bytes(socket.readAll())
                    callback(message)
                    socket.disconnectFromServer()

                socket.readyRead.connect(ready)
                socket.disconnected.connect(socket.deleteLater)
                if socket.bytesAvailable():
                    ready()

        self.server.newConnection.connect(connected)

    def close(self):
        self.server.close()
        self.lock.unlock()
