"""Keep a packaged bench session awake without changing system power settings."""
from contextlib import contextmanager
import os
import shutil
import signal
import subprocess
import sys


@contextmanager
def inhibit_sleep(*, enabled=True):
    inhibitor = shutil.which("systemd-inhibit") if enabled and sys.platform == "linux" else None
    process = None
    if inhibitor:
        command = [inhibitor, "--what=sleep:idle", "--who=KMSwitcher", "--why=Hardware test controller is open"]
        try:
            available = subprocess.run(command + ["true"], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=5).returncode == 0
            if available:
                # The pipe releases the lease even if the tester is killed: cat
                # sees EOF when the parent-owned write end closes. The server
                # remains the toolkit's direct child for graceful Stop handling.
                process = subprocess.Popen(command + ["cat"], stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        except (OSError, subprocess.TimeoutExpired):
            process = None
    if enabled and process is None:
        print("Automatic sleep inhibition is unavailable; keep this machine awake during tests.", flush=True)
    try:
        yield
    finally:
        if process:
            process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=5)
                except ProcessLookupError:
                    pass
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
