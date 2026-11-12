"""Thread-safe countdowns for timed runs and finite workload estimates."""
import threading
import time


class RunProgress:
    def __init__(self, seconds, test, timed=False):
        self.lock = threading.Lock()
        self.pending_seconds = seconds
        self.test = test
        self.timed = timed
        self.deadline = None
        self.phase = "preparing"
        self.slot = None
        self.status = None

    def start_timed(self, deadline):
        with self.lock:
            self.pending_seconds = 0
            self.deadline = deadline
            self.phase = "running"

    def begin_step(self, seconds, test, slot=None, extra=False):
        with self.lock:
            if not self.timed:
                if not extra:
                    self.pending_seconds = max(0, self.pending_seconds - seconds)
                self.deadline = time.monotonic() + seconds
            self.test, self.slot = test, slot
            self.phase = "running"

    def end_step(self):
        with self.lock:
            if not self.timed:
                self.deadline = None
            self.phase = "collecting"

    def cleanup(self):
        with self.lock:
            self.phase = "cleanup"

    def finish(self, status):
        with self.lock:
            self.phase = "finished"
            self.status = status
            self.pending_seconds = 0
            self.deadline = None

    def snapshot(self):
        with self.lock:
            remaining = self.pending_seconds
            if self.deadline is not None:
                remaining += max(0, self.deadline - time.monotonic())
            return dict(running=self.phase != "finished", phase=self.phase, status=self.status,
                        test=self.test, slot=self.slot, estimated=not self.timed,
                        remaining_seconds=remaining)
