"""Clipboard endpoint adapter; stream flow control lives in kms_transport."""
from dataclasses import replace
import queue
from kms_transport import Receipt, StreamLimits
from .clipboard_protocol import DATA_BYTES, Kind


class ClipboardDestination:
    def __init__(self, service, base, request=None, block=None):
        self.service, self.base, self.request = service, base, request
        self.block = block or (65536 if request else DATA_BYTES)
        self.replies = queue.Queue(maxsize=8)
        self.packets = {}

    def open(self, size, profile):
        response = (self.request or self.service._radio_request)(self.base)
        return response.offset, StreamLimits(self.block, window=1 if self.request else 4)

    def submit(self, offset, data):
        packet = replace(self.base, kind=Kind.DATA, sequence=self.service._sequence(), offset=offset, data=data)
        self.packets[packet.sequence] = packet
        if self.request:
            self.replies.put(self.request(packet))
        else:
            with self.service.waiter_lock:
                self.service.waiters[(packet.target, packet.target_session, packet.sequence)] = self.replies
            self.service._post(packet)
        return packet.sequence

    def retry(self, key):
        if self.request:
            self.replies.put(self.request(self.packets[key]))
        else:
            self.service._post(self.packets[key])

    def poll(self, timeout):
        try:
            reply = self.replies.get(timeout=timeout)
        except queue.Empty:
            return None
        if reply.sequence not in self.packets or reply.transfer != self.base.transfer:
            return None
        if reply.kind == Kind.ERROR:
            raise ValueError(reply.data.decode("utf-8", errors="replace"))
        if reply.kind != Kind.ACK:
            raise ValueError("Unexpected dongle transfer reply")
        return Receipt(reply.sequence, reply.offset)

    def release(self, key):
        packet = self.packets.pop(key)
        with self.service.waiter_lock:
            self.service.waiters.pop((packet.target, packet.target_session, key), None)

    def finish(self, size):
        reply = (self.request or self.service._radio_request)(replace(
            self.base, kind=Kind.END, sequence=self.service._sequence(), offset=size, data=b""))
        if reply.offset != size:
            raise ValueError("Destination did not commit the complete clipboard")

    def close(self):
        for key in list(self.packets):
            self.release(key)
