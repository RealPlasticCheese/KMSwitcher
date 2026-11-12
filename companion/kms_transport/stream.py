"""Adapters own framing and acknowledgements; callers supply a readable stream.

Only acknowledged bytes count as progress. At most window * block bytes are
read ahead, including retries. Neither the source nor its consumer knows MTUs.
"""
from dataclasses import dataclass
import time


@dataclass(frozen=True)
class StreamLimits:
    block: int
    window: int = 1
    retry: float = .8
    maximum_retry: float = 2
    stall: float = 30

    def __post_init__(self):
        if not 1 <= self.block <= 65536 or not 1 <= self.window <= 16:
            raise ValueError("Stream buffers exceed the transport budget")
        if not 0 < self.retry <= self.maximum_retry < self.stall:
            raise ValueError("Invalid stream retry policy")


@dataclass(frozen=True)
class Receipt:
    key: object
    offset: int


def send_stream(source, size, destination, *, profile="conservative", progress=lambda done, total: None,
                check=lambda: None, waiting=lambda: None, clock=time.monotonic):
    """Deliver a stream with bounded read-ahead, cancellation and capped backoff.

    open returns (committed offset, limits); submit/retry/poll/release handle the
    link. finish commits endpoint-specific integrity checks. close always runs.
    check may raise to cancel; the adapter closes its protocol session on abort.
    """
    pending = {}
    try:
        offset, limits = destination.open(size, profile)
        if not 0 <= offset <= size:
            raise ValueError("Invalid destination resume offset")
        source.seek(offset)
        sent = offset
        delay = limits.retry
        deadline, expires = clock() + delay, clock() + limits.stall
        while offset < size:
            check()
            if clock() >= expires:
                raise TimeoutError(f"Transfer made no progress for {limits.stall:g} seconds")
            while len(pending) < limits.window and sent < size:
                check()
                data = source.read(min(limits.block, size - sent))
                if not data or len(data) > min(limits.block, size - sent):
                    raise ValueError("Source stream changed during transfer")
                key = destination.submit(sent, data)
                if key in pending:
                    raise ValueError("Transport reused a pending receipt key")
                pending[key] = sent + len(data)
                sent += len(data)
            now = clock()
            receipt = destination.poll(max(0, min(.05, deadline - now, expires - now)))
            if receipt is None:
                if clock() < deadline:
                    continue
                check()
                for key in pending:
                    destination.retry(key)
                delay = min(limits.maximum_retry, delay * 2)
                deadline = clock() + delay
                waiting()
                continue
            if receipt.key not in pending or receipt.offset <= offset:
                continue
            if receipt.offset not in pending.values():
                raise ValueError("Destination acknowledged an unexpected offset")
            offset = receipt.offset
            for key, end in list(pending.items()):
                if end <= offset:
                    destination.release(key)
                    del pending[key]
            progress(offset, size)
            delay = limits.retry
            deadline, expires = clock() + delay, clock() + limits.stall
        check()
        destination.finish(size)
        return offset
    finally:
        for key in pending:
            destination.release(key)
        destination.close()
