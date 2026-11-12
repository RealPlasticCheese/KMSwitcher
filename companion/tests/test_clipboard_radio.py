"""Exercise the real transfer service over a paced, lossy dongle model without wall-clock sleeps."""
from collections import Counter
from dataclasses import replace
import heapq
from pathlib import Path
import queue
import random
import struct
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

from kms_companion.clipboard_content import build_bundle, file_digest
from kms_companion.clipboard_protocol import DATA_BYTES, Kind, Packet
from kms_companion.clipboard_service import (RADIO_ACK_LIMIT, RADIO_MAX_BACKOFF, RADIO_RECEIVE_BURST,
                                              RADIO_STALL_TIMEOUT, RADIO_WINDOW, TransferService)


class PacedDongles:
    """Model HID/radio round trips and 10 ms packet pacing; run the production receiver."""
    def __init__(self, sender, receiver):
        self.sender, self.receiver = sender, receiver
        self.now = time.monotonic()
        self.events = []
        self.order = 0
        self.next_send = {sender: self.now, receiver: self.now}
        self.sent = Counter()
        self.posts = []
        self.busy_until = 0
        self.refresh_discovery = True
        self.drop_data = set()
        self.drop_acks = set()
        self.drop_commit_ack = False
        self.duplicate_acks = False
        self.delay_data = {}
        self.drop_all_data = False
        self.after_data = lambda packet: None
        self.change_reply = lambda reply: reply
        self.max_waiters = 0

    def schedule(self, at, callback):
        self.order += 1
        heapq.heappush(self.events, (at, self.order, callback))

    def post(self, source, packet):
        target = self.receiver if source is self.sender else self.sender
        self.max_waiters = max(self.max_waiters, len(self.sender.waiters))
        self.sent[(source.slot, packet.kind, packet.offset)] += 1
        self.posts.append((self.now, packet))
        at = max(self.now, self.next_send[source])
        self.next_send[source] = at + .01
        if packet.kind == Kind.DATA:
            if self.drop_all_data:
                return
            if packet.offset in self.drop_data:
                self.drop_data.remove(packet.offset)
                return
        extra_delay = self.delay_data.pop(packet.offset, 0) if packet.kind == Kind.DATA else 0

        def receive():
            target.ui_seen = self.now
            session, advert, _ = target.peers[packet.source]
            if self.refresh_discovery:
                target.peers[packet.source] = (session, advert, self.now)
            if self.now < self.busy_until:
                return  # Input can occupy the link long enough to drop bulk/control traffic.
            if packet.kind == Kind.DATA:
                self.after_data(packet)
            # Exercise the unchanged radio codec as well as receiver offset handling.
            reply = target.handle(Packet.decode(packet.encode()), "dongle")
            if reply is None:
                return
            if packet.kind == Kind.DATA and reply.kind == Kind.ACK and reply.offset in self.drop_acks:
                self.drop_acks.remove(reply.offset)
                return
            if packet.kind == Kind.END and self.drop_commit_ack:
                self.drop_commit_ack = False
                return
            reply = self.change_reply(reply)
            back = max(self.now, self.next_send[target])
            self.next_send[target] = back + .01
            self.schedule(back + .06, lambda: source.deliver(reply))
            if self.duplicate_acks and reply.kind == Kind.ACK:
                self.schedule(back + .23, lambda: source.deliver(reply))

        self.schedule(at + .06 + extra_delay, receive)

    def reply_queue(self):
        link = self
        original_queue = queue.Queue

        class Replies(original_queue):
            def get(self, block=True, timeout=None):
                if not block or not self.empty():
                    return super().get(block=False)
                deadline = link.now + timeout
                while link.events and link.events[0][0] <= deadline:
                    link.now, _, callback = heapq.heappop(link.events)
                    callback()
                    if not self.empty():
                        return super().get(block=False)
                link.now = deadline
                raise queue.Empty

        return Replies


class RadioWindowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.a, self.b = TransferService(self.root / "a"), TransferService(self.root / "b")
        for service, slot, session in ((self.a, 1, 101), (self.b, 2, 202)):
            self.addCleanup(service.close)
            service.slot, service.session = slot, session
            service.desktop_ready = True
            service.ui_seen = time.monotonic()
            service.settings["mode"] = "dongle"
        self.a.peers[2] = (202, self.b.advertisement(1, 101), time.monotonic())
        self.b.peers[1] = (101, self.a.advertisement(2, 202), time.monotonic())
        self.source = self.root / "payload.bin"
        self.source.write_bytes(random.Random(42).randbytes(4096))
        self.bundle = self.root / "bundle.zip"
        build_bundle(self.bundle, {}, [self.source])
        self.base = Packet(Kind.BEGIN, 1, 2, 101, 202, 99, 123,
                           data=struct.pack("<I", self.bundle.stat().st_size) + bytes.fromhex(file_digest(self.bundle)))
        self.a._item(123, direction="send", slot=2, state="Sending", source_session=101, peer_session=202)
        self.link = PacedDongles(self.a, self.b)
        self.a._post = lambda packet: self.link.post(self.a, packet)
        self.b._post = lambda packet: self.link.post(self.b, packet)

    def transfer(self, windowed=True):
        replies = self.link.reply_queue()
        started = self.link.now
        with patch("kms_companion.clipboard_service.queue.Queue", replies), \
             patch("kms_companion.clipboard_service.time.monotonic", side_effect=lambda: self.link.now):
            if windowed:
                self.a._radio_stream(self.base, self.bundle)
            else:
                self.a._stream(self.base, self.bundle, self.a._radio_request, DATA_BYTES)
        self.assertFalse(self.a.waiters)
        self.assertLessEqual(self.link.max_waiters, RADIO_WINDOW)
        return self.link.now - started

    def assert_saved(self):
        item = next(iter(self.b.items.values()))
        self.assertEqual(item["state"], "Ready")
        saved = Path(self.b.save(item["id"], self.root)["path"])
        self.assertEqual((saved / self.source.name).read_bytes(), self.source.read_bytes())

    def test_window_reduces_round_trip_waiting_against_the_legacy_sender(self):
        legacy = self.transfer(windowed=False)
        old_incoming = next(iter(self.b.incoming.values()))
        self.b.dismiss(old_incoming["id"])
        self.base = replace(self.base, transfer=124)
        self.link = PacedDongles(self.a, self.b)
        windowed = self.transfer()
        self.assert_saved()
        self.assertLess(windowed, legacy / 2.5)
        self.assertEqual(self.link.max_waiters, RADIO_WINDOW)
        print(f"Dongle model: stop-and-wait {legacy:.3f}s, window {windowed:.3f}s, {legacy / windowed:.2f}x")

    def test_lost_data_and_late_duplicate_acks_preserve_file_integrity(self):
        self.link.drop_data = {160, 800}
        self.link.duplicate_acks = True
        self.transfer()
        self.assert_saved()
        self.assertGreater(self.link.sent[(1, Kind.DATA, 160)], 1)
        self.assertFalse(self.a.radio_in.qsize())

    def test_cumulative_ack_recovers_a_lost_receipt_without_resending_data(self):
        self.link.drop_acks = {320}
        self.transfer()
        self.assert_saved()
        self.assertTrue(all(count == 1 for (slot, kind, _), count in self.link.sent.items() if kind == Kind.DATA))

    def test_out_of_order_chunks_are_retried_from_the_contiguous_offset(self):
        self.link.delay_data = {160: .3}
        self.transfer()
        self.assert_saved()

    def test_resume_from_an_unaligned_network_offset_and_lost_commit_ack(self):
        self.a.settings["mode"] = self.b.settings["mode"] = "network-preferred"
        self.b.peers[1] = (101, self.a.advertisement(2, 202), self.link.now)
        self.assertEqual(self.b.handle(self.base, "network").kind, Kind.ACK)
        self.assertEqual(self.b.handle(replace(self.base, kind=Kind.DATA, data=self.bundle.read_bytes()[:81]), "network").offset, 81)
        self.a.settings["mode"] = self.b.settings["mode"] = "dongle"
        self.link.drop_commit_ack = True
        self.transfer()
        self.assert_saved()
        self.assertEqual(self.link.sent[(1, Kind.DATA, 0)], 0)
        self.assertEqual(self.link.sent[(1, Kind.DATA, 81)], 1)
        self.assertEqual(self.link.sent[(1, Kind.END, self.bundle.stat().st_size)], 2)

    def test_stalled_link_has_bounded_retries_and_cleans_all_waiters(self):
        self.link.drop_all_data = True
        started = self.link.now
        with self.assertRaises(TimeoutError):
            self.transfer()
        self.assertFalse(self.a.waiters)
        self.assertGreater(self.link.sent[(1, Kind.DATA, 0)], 5)
        self.assertLessEqual(self.link.now - started, RADIO_STALL_TIMEOUT + .2)
        times = [at for at, packet in self.link.posts if packet.kind == Kind.DATA and packet.offset == 0]
        for expected, left, right in zip((.8, 1.6, RADIO_MAX_BACKOFF, RADIO_MAX_BACKOFF), times, times[1:]):
            self.assertAlmostEqual(right - left, expected, places=4)
        self.assertLessEqual(self.link.max_waiters, RADIO_WINDOW)
        self.assertFalse(next(iter(self.b.incoming.values())).get("manifest"))

    def test_input_congestion_and_stale_discovery_pause_instead_of_failing(self):
        self.link.refresh_discovery = False
        def pause(packet):
            if not self.link.busy_until:
                self.link.busy_until = self.link.now + 10
        self.link.after_data = pause
        elapsed = self.transfer()
        self.assertGreater(elapsed, 10)
        self.assert_saved()
        self.assertFalse(self.a.items[123].get("detail"))

    def test_commit_can_wait_for_input_congestion_with_stale_discovery(self):
        self.link.refresh_discovery = False
        size = self.bundle.stat().st_size
        def pause(packet):
            if packet.offset + len(packet.data) == size and not self.link.busy_until:
                self.link.busy_until = self.link.now + 10
        self.link.after_data = pause
        self.assertGreater(self.transfer(), 10)
        self.assert_saved()
        self.assertGreater(self.link.sent[(1, Kind.END, size)], 5)

    def test_initial_request_retries_input_congestion_with_backoff(self):
        self.link.busy_until = self.link.now + 10
        self.assertGreater(self.transfer(), 10)
        self.assert_saved()
        times = [at for at, packet in self.link.posts if packet.kind == Kind.BEGIN]
        self.assertGreater(len(times), 5)
        for expected, left, right in zip((.8, 1.6, RADIO_MAX_BACKOFF), times, times[1:]):
            self.assertAlmostEqual(right - left, expected, places=4)

    def test_cancellation_is_prompt_during_backoff(self):
        self.link.drop_all_data = True
        started = self.link.now
        self.link.schedule(started + 5, lambda: self.a.cancel(self.base.transfer))
        with self.assertRaisesRegex(ValueError, "cancelled"):
            self.transfer()
        self.assertLess(self.link.now - started, 5.1)
        self.assertFalse(self.a.waiters)

    def test_cancel_from_recipient_stops_a_full_window(self):
        def cancel(packet):
            self.b.cancel(next(iter(self.b.incoming.values()))["id"])
        self.link.after_data = cancel
        with self.assertRaisesRegex(ValueError, "cancelled"):
            self.transfer()
        self.assertFalse(self.a.waiters)
        self.assertIn(123, self.a.cancelled)
        self.assertEqual(next(iter(self.b.items.values()))["progress"], 0)

    def test_impossible_ack_offset_is_rejected(self):
        self.link.change_reply = lambda reply: replace(reply, offset=161) if reply.offset else reply
        with self.assertRaisesRegex(ValueError, "unexpected offset"):
            self.transfer()
        self.assertFalse(self.a.waiters)

    def assert_window_aborts(self, change):
        self.link.after_data = lambda packet: change()
        with self.assertRaises((ConnectionError, ValueError)):
            self.transfer()
        self.assertFalse(self.a.waiters)

    def test_session_change_aborts_the_window(self):
        self.assert_window_aborts(lambda: setattr(self.a, "session", 303))

    def test_network_only_policy_aborts_the_window(self):
        self.assert_window_aborts(lambda: self.a.settings.update(mode="network"))

    def test_sender_cancel_stops_a_full_window(self):
        self.assert_window_aborts(lambda: self.a.cancel(self.base.transfer))
        self.assertEqual(self.a.items[self.base.transfer]["state"], "Cancelled")
        self.assertEqual(sum(count for (slot, kind, _), count in self.link.sent.items() if kind == Kind.DATA), RADIO_WINDOW)

    def test_pending_data_acknowledgements_coalesce_without_losing_control_replies(self):
        self.b._post = lambda packet: TransferService._post(self.b, packet)
        begin = self.b.handle(self.base, "dongle")
        self.b._queue_radio_reply(self.base, begin)
        data = self.bundle.read_bytes()
        stream = None
        for offset in range(0, len(data), DATA_BYTES):
            request = replace(self.base, kind=Kind.DATA, sequence=100 + offset, offset=offset,
                              data=data[offset:offset + DATA_BYTES])
            reply = self.b.handle(request, "dongle")
            self.assertEqual(reply.kind, Kind.ACK)
            self.b._queue_radio_reply(request, reply)
            current = next(iter(self.b.incoming.values()))["stream"]
            if stream is None:
                stream = current
            self.assertIs(current, stream)
        end = replace(self.base, kind=Kind.END, sequence=999, data=b"")
        end_reply = self.b.handle(end, "dongle")
        self.b._queue_radio_reply(end, end_reply)
        self.assertTrue(stream.closed)
        self.assertEqual(len(self.b.radio_acks), 1)
        packets = [self.b._take_radio_packet() for _ in range(3)]
        self.assertIn(begin, packets)
        self.assertIn(end_reply, packets)
        cumulative = next(packet for packet in packets if packet.sequence == request.sequence)
        self.assertEqual((cumulative.sequence, cumulative.offset), (request.sequence, len(data)))
        with self.assertRaises(queue.Empty):
            self.b._take_radio_packet()
        self.assert_saved()

    def test_ack_coalescer_has_a_fixed_memory_bound(self):
        self.b._post = lambda packet: TransferService._post(self.b, packet)
        for transfer in range(RADIO_ACK_LIMIT + 4):
            request = replace(self.base, kind=Kind.DATA, transfer=1000 + transfer)
            self.b._queue_radio_reply(request, request.reply(Kind.ACK, offset=160))
        self.assertEqual(len(self.b.radio_acks), RADIO_ACK_LIMIT)
        self.assertEqual(self.b.radio_out.qsize(), 4)

    def test_ack_and_send_queues_both_progress_during_bidirectional_traffic(self):
        self.b._post = lambda packet: TransferService._post(self.b, packet)
        taken = []
        for transfer in range(8):
            request = replace(self.base, kind=Kind.DATA, transfer=1000 + transfer)
            self.b._queue_radio_reply(request, request.reply(Kind.ACK, offset=160))
            self.b._post(self.base.reply(Kind.INFO))
            taken.append(self.b._take_radio_packet().kind)
        self.assertEqual(taken.count(Kind.INFO), 4)
        self.assertEqual(taken.count(Kind.ACK), 4)

    def test_active_transfer_discovery_does_not_probe_or_block_the_hid_owner(self):
        self.a.radio_target = (2, 202)
        self.a._post = Mock()
        receiver = Mock(session=101)
        receiver.clipboard_receive.return_value = None
        receiver.stream_capacity.return_value = 1
        self.a.pump(receiver, dict(slot=1, clipboard=True, usb_ready=True), True)
        receiver.clipboard_peer.assert_not_called()
        query = self.a._post.call_args.args[0]
        self.assertEqual((query.kind, query.target, query.target_session), (Kind.QUERY, 2, 202))

    def test_hid_receive_burst_is_bounded_and_sends_only_one_packet(self):
        receiver = Mock(session=101)
        receiver.clipboard_receive.return_value = self.base.reply(Kind.INFO)
        self.a.next_scan = float("inf")
        self.a.radio_out.put_nowait(self.base)
        with patch("kms_companion.clipboard_service.time.monotonic", return_value=self.link.now):
            receiver.stream_capacity.return_value = 1
            self.a.pump(receiver, dict(slot=1, clipboard=True, usb_ready=True), True)
        self.assertEqual(receiver.clipboard_receive.call_count, RADIO_RECEIVE_BURST)
        receiver.clipboard_send.assert_called_once_with(self.base)

    def test_slow_hid_reads_stop_at_the_receive_time_budget(self):
        receiver = Mock(session=101)
        receiver.clipboard_receive.return_value = self.base.reply(Kind.INFO)
        self.a.next_scan = float("inf")
        with patch("kms_companion.clipboard_service.time.monotonic", side_effect=[0, .003, .003, .003]):
            receiver.stream_capacity.return_value = 1
            self.a.pump(receiver, dict(slot=1, clipboard=True, usb_ready=True), True)
        receiver.clipboard_receive.assert_called_once()

    def test_discovery_of_a_new_destination_session_aborts_and_clears_active_target(self):
        session, advert, seen = self.a.peers[2]
        self.assert_window_aborts(lambda: self.a.peers.update({2: (999, advert, seen)}))
        self.assertIsNone(self.a.radio_target)
