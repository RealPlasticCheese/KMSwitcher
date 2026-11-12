"""One hardware operation per user across toolkit testing and firmware updates."""
from contextlib import contextmanager
import getpass
import hashlib
import os
from pathlib import Path
import tempfile


@contextmanager
def fixture_lock():
    identity = hashlib.sha256(getpass.getuser().encode()).hexdigest()[:16]
    path = Path(tempfile.gettempdir()) / f"kmswitcher-hardware-{identity}.lock"
    stream = path.open("a+b")
    try:
        if os.name == "nt":
            import msvcrt
            stream.seek(0); stream.write(b"0"); stream.flush(); stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as error:
        stream.close()
        raise RuntimeError("Another KMSwitcher test or firmware operation is open. Close it before continuing.") from error
    try:
        yield
    finally:
        if os.name == "nt":
            stream.seek(0); msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        stream.close()
