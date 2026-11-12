"""Authenticated local JSON IPC, with an OS lock for exclusive dongle ownership."""
import hashlib
import json
from multiprocessing.connection import Client, Listener
import os
from pathlib import Path
import secrets
import threading
from .agent import config_directory


def endpoint(directory):
    if os.name == "nt":
        suffix = hashlib.sha256(str(directory).encode()).hexdigest()[:16]
        return r"\\.\pipe\kmswitcher-" + suffix, "AF_PIPE"
    return str(directory / "agent.sock"), "AF_UNIX"


def call(command, **args):
    directory = config_directory()
    try:
        key = (directory / "ipc.key").read_bytes()
        address, family = endpoint(directory)
        with Client(address, family=family, authkey=key) as conn:
            conn.send_bytes(json.dumps({"command": command, "args": args}).encode())
            if not conn.poll(25):
                raise RuntimeError("Companion command timed out")
            reply = json.loads(conn.recv_bytes(1024 * 1024))
    except (OSError, EOFError) as error:
        raise RuntimeError("Background helper is not running; start 'kmswitcher run' or 'kmswitcher ui'") from error
    if "error" in reply:
        raise RuntimeError(reply["error"])
    return reply["result"]


class Server:
    def __init__(self, agent):
        self.agent = agent
        self.directory = agent.directory
        if os.name != "nt":
            self.directory.chmod(0o700)
        self.lock_file = (self.directory / "instance.lock").open("a+b")
        self.lock_file.seek(0)
        self.lock_file.write(b"0")
        self.lock_file.flush()
        self.lock_file.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.lock_file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            self.lock_file.close()
            raise RuntimeError("A KMSwitcher helper is already running") from error
        key_file = self.directory / "ipc.key"
        if not key_file.exists():
            fd = os.open(key_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(secrets.token_bytes(32))
        address, family = endpoint(self.directory)
        if family == "AF_UNIX":
            Path(address).unlink(missing_ok=True)
        self.listener = Listener(address, family=family, authkey=key_file.read_bytes())

    def start(self):
        self.agent.start()
        threading.Thread(target=self.serve, daemon=True, name="kms-local-ipc").start()

    def serve(self):
        while not self.agent.stop_event.is_set():
            try:
                conn = self.listener.accept()
                threading.Thread(target=self.handle, args=(conn,), daemon=True).start()
            except (OSError, EOFError):
                return

    def handle(self, conn):
        with conn:
            try:
                if not conn.poll(5):
                    return
                request = json.loads(conn.recv_bytes(1024 * 1024))
                result = self.agent.call(request["command"], **request.get("args", {}))
                conn.send_bytes(json.dumps({"result": result}).encode())
            except Exception as error:
                conn.send_bytes(json.dumps({"error": str(error)}).encode())

    def close(self):
        self.agent.close()
        self.listener.close()
        self.lock_file.close()
