"""Real bundle, protocol, encrypted TCP and Qt clipboard interoperability checks."""
from dataclasses import replace
from datetime import datetime
import contextlib
import ctypes
import importlib.util
import json
import os
import queue
from pathlib import Path
import secrets
import socket
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile
from kms_companion.clipboard_content import build_bundle, file_digest, safe_name, unpack_bundle
from kms_companion.clipboard_protocol import (Advertisement, Kind, Packet, SecureRecords, transports,
                                             LAN_HEADER, network_key)
from kms_companion.clipboard_service import TransferService

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


class ProtocolTests(unittest.TestCase):
    def test_radio_and_large_network_roundtrip(self):
        for radio, size in ((True, 160), (False, 65536)):
            p = Packet(Kind.DATA, 1, 2, 101, 202, 3, 4, 160, secrets.token_bytes(size))
            self.assertEqual(Packet.decode(p.encode(radio), radio), p)
        p = replace(p, data=b"x")
        for index in range(192):
            corrupt = bytearray(p.encode()); corrupt[index] ^= 1
            with self.assertRaises(ValueError):
                Packet.decode(corrupt)

    def test_transport_preferences_are_bilateral(self):
        self.assertEqual(transports("dongle", "network-preferred"), ["dongle"])
        self.assertEqual(transports("network-preferred", "network-preferred"), ["network", "dongle"])
        self.assertEqual(transports("network", "network-preferred"), ["network"])
        self.assertEqual(transports("network", "dongle"), [])

    def test_advertisement_preserves_computer_identity(self):
        advert = Advertisement("network-preferred", True, 1234, secrets.token_bytes(32), "Laptop 😀", ("192.168.1.10",))
        for advert in (advert, replace(advert, machine_id=secrets.token_bytes(16))):
            self.assertEqual(Advertisement.decode(advert.encode()), advert)

    def test_python_packets_roundtrip_through_production_c(self):
        dll = Path(__file__).resolve().parents[2] / "build/desktop-tests/desktop-tests.dll"
        if not dll.exists():
            self.skipTest("Run scripts/test-desktop.py first")
        native = ctypes.CDLL(str(dll))
        p = Packet(Kind.DATA, 1, 2, 101, 202, 3, 4, 160, bytes(range(160))).encode()
        output = ctypes.create_string_buffer(192)
        self.assertEqual(native.kms_clipboard_sim_codec(p, output), 1)
        self.assertEqual(output.raw, p)

    def test_secure_records_are_directional_authenticated_and_ordered(self):
        key, header = secrets.token_bytes(32), b"session-header"
        with contextlib.ExitStack() as stack:
            left, right = socket.socketpair()
            stack.enter_context(left); stack.enter_context(right)
            client, server = SecureRecords(left, key, header, True), SecureRecords(right, key, header, False)
            p = Packet(Kind.DATA, 1, 2, 101, 202, 3, data=b"secret")
            client.send(p); self.assertEqual(server.receive(), p)
            server.send(p.reply(Kind.ACK)); self.assertEqual(client.receive(), p.reply(Kind.ACK))
            client.sent = 0
            client.send(p)
            with self.assertRaisesRegex(ValueError, "Replayed"):
                server.receive()

    def test_wrong_network_key_cannot_supply_content(self):
        with contextlib.ExitStack() as stack:
            left, right = socket.socketpair()
            stack.enter_context(left); stack.enter_context(right)
            client = SecureRecords(left, b"x" * 32, b"header", True)
            server = SecureRecords(right, b"y" * 32, b"header", False)
            client.send(Packet(Kind.DATA, 1, 2, 101, 202, 3, data=b"secret"))
            with self.assertRaisesRegex(ValueError, "authentication"):
                server.receive()


class ContentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_unicode_and_rich_formats_have_distinct_acceptance(self):
        for index, formats in enumerate(({"text/plain": "Hello 😀\n世界".encode()},
                {"text/plain": b"Hello", "text/html": b"<b>Hello</b>", "text/rtf": b"{\\rtf1 Hello}"})):
            bundle = self.root / f"{index}.zip"
            build_bundle(bundle, formats)
            target = self.root / f"unpacked{index}"
            manifest = unpack_bundle(bundle, target)
            self.assertEqual(manifest["kind"], "text" if index == 0 else "content")
            for mime, data in formats.items():
                self.assertEqual((target / manifest["formats"][mime]).read_bytes(), data)

    def test_files_folders_empty_directories_and_snapshot(self):
        source = self.root / "Folder"; source.mkdir(); (source / "empty").mkdir()
        (source / "file.txt").write_text("before", encoding="utf-8")
        bundle = self.root / "files.zip"
        build_bundle(bundle, {}, [source])
        (source / "file.txt").write_text("after", encoding="utf-8")
        target = self.root / "unpacked"
        manifest = unpack_bundle(bundle, target)
        self.assertEqual(manifest["kind"], "files")
        self.assertEqual((target / "files/Folder/file.txt").read_text(), "before")
        self.assertTrue((target / "files/Folder/empty").is_dir())

    def test_unsafe_paths_duplicate_names_and_size_limits(self):
        for name in ("../escape", "/absolute", "C:/drive", "files/CON", "files/x.", "files/a\\b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                safe_name(name)
        with self.assertRaises(ValueError):
            build_bundle(self.root / "large.zip", {"text/plain": b"12345"}, limit=4)
        bundle = self.root / "bad.zip"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("../escape", b"bad")
        with self.assertRaises(ValueError):
            unpack_bundle(bundle, self.root / "bad")
        self.assertFalse((self.root / "bad").exists())

    def test_manifest_cannot_override_validated_content_classification(self):
        bundle = self.root / "bundle.zip"
        with zipfile.ZipFile(bundle, "w") as archive:
            archive.writestr("formats/a", b"Hello")
            archive.writestr("formats/b", b"<b>Hello</b>")
            archive.writestr("manifest.json", json.dumps(dict(schema=1, kind="text", roots=[],
                formats={"text/plain": "formats/a", "text/html": "formats/b"})))
        self.assertEqual(unpack_bundle(bundle, self.root / "unpacked")["kind"], "content")


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.a = TransferService(self.root / "A"); self.b = TransferService(self.root / "B")
        for service, slot, session in ((self.a, 1, 101), (self.b, 2, 202)):
            self.addCleanup(service.close)
            service.slot, service.session = slot, session
            service.desktop_ready = True; service.ui_seen = time.monotonic()
            service.settings["mode"] = "network-preferred"
        self.a.peers[2] = (202, self.b.advertisement(1, 101), time.monotonic())
        self.b.peers[1] = (101, self.a.advertisement(2, 202), time.monotonic())
        self.bundle = self.root / "bundle.zip"
        build_bundle(self.bundle, {"text/plain": "Hello 😀".encode()})
        self.base = Packet(Kind.BEGIN, 1, 2, 101, 202, 99, 123,
            data=struct.pack("<I", self.bundle.stat().st_size) + bytes.fromhex(file_digest(self.bundle)))

    def request(self, packet, transport="dongle"):
        response = self.b.handle(packet, transport)
        if response.kind == Kind.ERROR:
            raise ValueError(response.data.decode())
        return response

    def test_machine_identity_is_disclosed_to_new_discovery_requests(self):
        self.b.machine_id = b"b" * 16
        query = self.a._peer_query(2, 202)
        self.assertEqual(Advertisement.decode(query.data).machine_id, b"")
        self.assertEqual(self.a._remember_peer(self.request(query)).machine_id, self.b.machine_id)
        legacy_reply = self.request(replace(query, offset=0))
        self.assertEqual(Advertisement.decode(legacy_reply.data).machine_id, b"")

    def test_automatic_fanout_skips_unselected_and_reuses_one_file_snapshot(self):
        self.b.machine_id = b"b" * 16
        c = TransferService(self.root / "C")
        self.addCleanup(c.close)
        c.slot, c.session, c.desktop_ready, c.machine_id = 4, 404, True, b"c" * 16
        c.ui_seen = time.monotonic()
        self.a.settings.update(mode="dongle", automatic_send=dict(files="selected"), automatic_targets={
            peer.machine_id.hex(): peer.settings["name"] for peer in (self.b, c)})
        destinations = {2: self.b, 4: c}
        for slot, peer in destinations.items():
            self.a.peers[slot] = (peer.session, peer.advertisement(1, 101, True), time.monotonic())
        # The old dock now has a different computer with the same display name.
        self.a.peers[3] = (303, replace(self.a.peers[4][1], machine_id=b"d" * 16), time.monotonic())
        file = self.root / "shared.txt"
        file.write_text("original", encoding="utf-8")

        def make_capture(name):
            capture = self.a.root / "outgoing" / name / "capture.json"
            capture.parent.mkdir()
            capture.write_text(json.dumps(dict(formats={}, files=[str(file)])), encoding="utf-8")
            return capture

        self.assertTrue(self.a.send_automatic(make_capture("old"), 101)["queued"])
        old = self.a.pending_copy.capture
        self.assertTrue(self.a.send_automatic(make_capture("new"), 101)["queued"])
        self.assertFalse(old.exists())
        copy, self.a.pending_copy = self.a.pending_copy, None
        deliveries = []

        def transfer(base, bundle):
            target = destinations[base.target]
            self.a._stream(base, bundle, lambda packet: target.handle(packet, "dongle"), 160)
            incoming = next(iter(target.incoming.values()))
            deliveries.append((base.target, (incoming["directory"] / "content" / "files" / file.name).read_text()))
            file.write_text("modified", encoding="utf-8")

        with patch.object(self.a, "_radio_request", side_effect=lambda p: destinations[p.target].handle(p, "dongle")), \
             patch.object(self.a, "_radio_stream", side_effect=transfer):
            list(self.a._send_automatic_copy(copy))
        self.assertEqual(deliveries, [(2, "original"), (4, "original")])
        self.assertFalse(copy.capture.exists())
        self.assertTrue(all(item["state"] == "Complete" for item in self.a.items.values()))

    def test_automatic_send_uses_the_content_type_policy(self):
        self.b.machine_id = b"b" * 16
        self.a.peers[2] = (202, self.b.advertisement(1, 101, True), time.monotonic())
        self.a.configure(dict(self.a.settings, automatic_send=dict(text="all", rich="selected")))
        for formats, files, expected in (
                ({"text/plain": "plain"}, [], True),
                ({"text/plain": "plain", "text/html": "rich"}, [], False),
                ({"text/plain": "plain", "text/html": "rich", "image/png": "image"}, [], False),
                ({"text/plain": "plain"}, ["some-file"], False)):
            with self.subTest(formats=formats, files=files):
                folder = self.a.root / "outgoing" / secrets.token_hex(8)
                folder.mkdir()
                capture = folder / "capture.json"
                capture.write_text(json.dumps(dict(formats=formats, files=files)), encoding="utf-8")
                self.assertEqual(self.a.send_automatic(capture, 101)["queued"], expected)
                self.assertFalse(folder.exists())

    def test_duplicate_chunks_and_repeated_commit_are_idempotent(self):
        self.request(self.base)
        chunk = replace(self.base, kind=Kind.DATA, data=self.bundle.read_bytes())
        first = self.request(chunk); second = self.request(chunk)
        self.assertEqual(first.offset, second.offset)
        end = replace(self.base, kind=Kind.END, data=b"")
        self.request(end); self.request(end)
        self.assertEqual(len(self.b.incoming), 1)
        item = next(iter(self.b.items.values()))
        self.assertEqual((item["state"], item["kind"]), ("Ready", "text"))

    def test_network_to_dongle_fallback_resumes_without_duplicate_delivery(self):
        self.request(self.base, "network")
        self.request(replace(self.base, kind=Kind.DATA, data=self.bundle.read_bytes()[:80]), "network")
        self.a._stream(self.base, self.bundle, self.request, 160)
        self.assertEqual(len(self.b.incoming), 1)
        self.assertEqual(next(iter(self.b.items.values()))["state"], "Ready")

    def _file_bundle(self):
        source = self.root / "received.txt"
        source.write_text("Complete file content", encoding="utf-8")
        build_bundle(self.bundle, {}, [source])
        self.base = replace(self.base, data=struct.pack("<I", self.bundle.stat().st_size) +
                            bytes.fromhex(file_digest(self.bundle)))
        return source

    def _assert_received_file_can_be_saved(self, source):
        item = next(iter(self.b.items.values()))
        self.assertEqual((item["state"], item["kind"]), ("Ready", "files"))
        self.assertFalse(item.get("detail"))
        saved = Path(self.b.save(item["id"], self.root)["path"])
        self.assertEqual((saved / source.name).read_bytes(), source.read_bytes())
        self.assertEqual(next(i for i in self.b.status()["items"] if i["id"] == item["id"])["saved_path"], str(saved))

    def test_only_successful_saves_update_the_location(self):
        source = self._file_bundle()
        self.a._stream(self.base, self.bundle, self.request, 160)
        item = next(iter(self.b.items.values()))
        with patch("kms_companion.clipboard_service.save_files", side_effect=OSError("Disk full")):
            with self.assertRaises(OSError):
                self.b.save(item["id"], self.root)
        self.assertNotIn("saved_path", item)
        self._assert_received_file_can_be_saved(source)
        saved_path = item["saved_path"]
        with self.assertRaises(FileExistsError):
            self.b.save(item["id"], self.root)
        self.assertEqual(item["saved_path"], saved_path)
        second_parent = self.root / "another location"
        second_parent.mkdir()
        result = self.b.save(item["id"], second_parent)
        self.assertNotEqual(result["path"], saved_path)
        self.assertEqual(item["saved_path"], result["path"])

    def test_receiving_recovers_from_an_earlier_packet_error_and_can_save(self):
        source = self._file_bundle()
        self.request(self.base)
        item = next(iter(self.b.items.values()))
        started = item["started"]
        response = self.b.handle(replace(self.base, kind=Kind.DATA, data=b""), "dongle")
        self.assertEqual(response.kind, Kind.ERROR)
        self.assertEqual(item["state"], "Failed")

        data = self.bundle.read_bytes()
        self.request(replace(self.base, kind=Kind.DATA, data=data[:160]))
        self.assertEqual(item["state"], "Receiving")
        self.assertFalse(item.get("detail"))
        self.assertEqual(item["started"], started)
        self.a._stream(self.base, self.bundle, self.request, 160)
        self._assert_received_file_can_be_saved(source)

    def test_late_packet_error_does_not_hide_a_verified_file(self):
        source = self._file_bundle()
        self.a._stream(self.base, self.bundle, self.request, 160)
        response = self.b.handle(replace(self.base, data=b"invalid header"), "dongle")
        self.assertEqual(response.kind, Kind.ERROR)
        self._assert_received_file_can_be_saved(source)

    def test_delayed_replies_are_not_treated_as_new_transfer_requests(self):
        self.request(self.base)
        chunk = replace(self.base, kind=Kind.DATA, data=self.bundle.read_bytes()[:160])
        ack = self.request(chunk)
        # A duplicate ACK can arrive after its request waiter has been removed.
        self.a.deliver(ack)
        self.assertTrue(self.a.radio_in.empty())
        # Otherwise the sender rejects that ACK and its ERROR fails the receiver.
        error = replace(chunk, kind=Kind.ERROR, data=b"Begin the clipboard transfer first")
        self.b.deliver(error)
        self.assertTrue(self.b.radio_in.empty())
        self.assertEqual(next(iter(self.b.items.values()))["state"], "Receiving")
        self.assertIsNone(self.b.handle(error, "dongle"))

    def test_unsolicited_info_still_updates_peer_discovery(self):
        packet = self.base.reply(Kind.INFO, self.b.advertisement(1, 101).encode())
        self.a.deliver(packet)
        self.a.peers.clear()
        self.assertIsNone(self.a.handle(self.a.radio_in.get_nowait(), "dongle"))
        self.assertEqual(self.a.peers[2][0], 202)

    def test_receiver_can_cancel_a_failed_but_unfinished_transfer(self):
        self.a._item(self.base.transfer, direction="send", slot=2, state="Sending",
                     source_session=101, peer_session=202)
        self.request(self.base)
        item = next(iter(self.b.items.values()))
        self.b.handle(replace(self.base, kind=Kind.DATA, data=b""), "dongle")
        self.assertTrue(item["cancellable"])
        self.b.cancel(item["id"])
        cancel = self.b.radio_out.get_nowait()
        self.assertEqual((cancel.kind, cancel.transfer), (Kind.CANCEL, self.base.transfer))
        self.assertEqual((cancel.target, cancel.target_session), (1, 101))
        self.assertEqual(self.a.handle(cancel, "dongle").kind, Kind.ACK)
        self.assertEqual(self.a.items[self.base.transfer]["state"], "Cancelled")
        with self.assertRaisesRegex(ValueError, "cancelled"):
            self.a._check_active(self.base)
        self.assertEqual(item["state"], "Cancelled")
        self.assertFalse(item["cancellable"])
        self.assertEqual(self.b.handle(replace(self.base, kind=Kind.DATA, data=b"x"), "dongle").kind, Kind.ERROR)
        self.assertEqual(item["progress"], 0)

    def test_sender_cancel_notifies_receiver_immediately(self):
        self.a._item(self.base.transfer, direction="send", slot=2, state="Sending",
                     source_session=101, peer_session=202)
        self.request(self.base)
        self.a.cancel(self.base.transfer)
        cancel = self.a.radio_out.get_nowait()
        self.request(cancel)
        self.assertEqual(next(iter(self.b.items.values()))["state"], "Cancelled")

    def test_recipient_cancel_cannot_cancel_a_send_to_another_session(self):
        self.a._item(self.base.transfer, direction="send", slot=2, state="Sending",
                     source_session=101, peer_session=999)
        self.a.handle(self.base.reply(Kind.CANCEL), "dongle")
        self.assertNotIn(self.base.transfer, self.a.cancelled)
        self.assertEqual(self.a.items[self.base.transfer]["state"], "Sending")

    def test_cancel_racing_with_commit_preserves_the_verified_file(self):
        source = self._file_bundle()
        self.a._stream(self.base, self.bundle, self.request, 160)
        identifier = next(iter(self.b.items))
        self.b.cancel(identifier)
        self._assert_received_file_can_be_saved(source)

    def test_commit_can_retry_after_desktop_locks_during_validation(self):
        source = self._file_bundle()
        self.request(self.base)
        data = self.bundle.read_bytes()
        for offset in range(0, len(data), 160):
            self.request(replace(self.base, kind=Kind.DATA, offset=offset, data=data[offset:offset + 160]))

        def lock_after_unpack(*args):
            manifest = unpack_bundle(*args)
            self.b.desktop_ready = False
            return manifest

        end = replace(self.base, kind=Kind.END, data=b"")
        with patch("kms_companion.clipboard_service.unpack_bundle", side_effect=lock_after_unpack):
            self.assertEqual(self.b.handle(end, "dongle").kind, Kind.ERROR)
        incoming = next(iter(self.b.incoming.values()))
        self.assertNotIn("manifest", incoming)
        self.assertFalse((incoming["directory"] / "content").exists())

        self.b.desktop_ready = True
        self.request(end)
        self._assert_received_file_can_be_saved(source)

    def test_session_change_lock_policy_and_integrity_fail_closed(self):
        self.b.session = 203
        self.assertEqual(self.b.handle(self.base, "dongle").kind, Kind.ERROR)
        self.b.session = 202; self.b.desktop_ready = False
        self.assertEqual(self.b.handle(self.base, "dongle").kind, Kind.ERROR)
        self.b.desktop_ready = True; self.b.settings["mode"] = "network"
        self.assertEqual(self.b.handle(self.base, "dongle").kind, Kind.ERROR)
        self.b.settings["mode"] = "network-preferred"
        self.request(self.base)
        self.request(replace(self.base, kind=Kind.DATA, data=b"x" * self.bundle.stat().st_size))
        self.assertEqual(self.b.handle(replace(self.base, kind=Kind.END), "dongle").kind, Kind.ERROR)
        self.assertFalse(any(i.get("state") == "Ready" for i in self.b.items.values()))

    def test_established_transfer_survives_delayed_discovery(self):
        source = self._file_bundle()
        self.request(self.base)
        session, advert, _ = self.b.peers[1]
        self.b.peers[1] = (session, advert, time.monotonic() - 10)
        self.a._stream(self.base, self.bundle, self.request, 160)
        self._assert_received_file_can_be_saved(source)

    def test_stale_discovery_cannot_authorize_a_new_transfer(self):
        session, advert, _ = self.b.peers[1]
        self.b.peers[1] = (session, advert, time.monotonic() - 10)
        self.assertEqual(self.b.handle(self.base, "dongle").kind, Kind.ERROR)
        self.assertFalse(self.b.incoming)

    def test_established_transfer_still_rejects_locked_desktop_and_changed_peer(self):
        self.request(self.base)
        session, advert, _ = self.b.peers[1]
        self.b.peers[1] = (session, advert, time.monotonic() - 10)
        chunk = replace(self.base, kind=Kind.DATA, data=self.bundle.read_bytes()[:160])
        self.b.desktop_ready = False
        self.assertEqual(self.b.handle(chunk, "dongle").kind, Kind.ERROR)
        self.b.desktop_ready = True
        self.b.peers[1] = (999, advert, time.monotonic())
        self.assertEqual(self.b.handle(chunk, "dongle").kind, Kind.ERROR)
        self.assertEqual(next(iter(self.b.incoming.values()))["offset"], 0)

    def test_expired_incoming_cannot_bypass_stale_discovery(self):
        from kms_companion.clipboard_service import RECEIVE_TTL
        self.request(self.base)
        incoming = next(iter(self.b.incoming.values()))
        incoming["seen"] -= RECEIVE_TTL + 1
        session, advert, _ = self.b.peers[1]
        self.b.peers[1] = (session, advert, time.monotonic() - 10)
        self.assertEqual(self.b.handle(replace(self.base, kind=Kind.DATA, data=b"x"), "dongle").kind, Kind.ERROR)
        self.assertEqual(incoming["offset"], 0)

    def test_buffered_file_uses_one_handle_and_flushes_before_verification(self):
        source = self._file_bundle()
        self.request(self.base)
        data = self.bundle.read_bytes()
        self.request(replace(self.base, kind=Kind.DATA, data=data[:80]))
        incoming = next(iter(self.b.incoming.values()))
        stream = incoming["stream"]
        self.a._stream(self.base, self.bundle, self.request, 160)
        self.assertTrue(stream.closed)
        self.assertNotIn("stream", incoming)
        self._assert_received_file_can_be_saved(source)

    def test_staging_handles_close_on_cancel_dismiss_expiry_and_shutdown(self):
        from kms_companion.clipboard_service import RECEIVE_TTL
        for index, action in enumerate(("cancel", "dismiss", "expire", "close")):
            with self.subTest(action=action):
                base = replace(self.base, transfer=123 + index)
                self.request(base)
                self.request(replace(base, kind=Kind.DATA, data=self.bundle.read_bytes()[:80]))
                incoming = self.b.incoming[(1, 101, base.transfer)]
                stream = incoming["stream"]
                if action == "expire":
                    incoming["seen"] -= RECEIVE_TTL + 1
                    self.b.next_addresses = float("inf")
                    self.b._expire()
                elif action == "close":
                    self.b.close()
                else:
                    getattr(self.b, action)(incoming["id"])
                self.assertTrue(stream.closed)
                self.assertNotIn("stream", incoming)

    def test_bound_recipient_cancel_survives_stale_discovery(self):
        self.a._item(self.base.transfer, direction="send", slot=2, state="Sending",
                     source_session=101, peer_session=202)
        session, advert, _ = self.a.peers[2]
        self.a.peers[2] = (session, advert, time.monotonic() - 10)
        self.assertEqual(self.a.handle(self.base.reply(Kind.CANCEL), "dongle").kind, Kind.ACK)
        self.assertEqual(self.a.items[self.base.transfer]["state"], "Cancelled")

    def test_cancel_and_receive_quota(self):
        self.request(self.base)
        item = next(iter(self.b.items.values())); self.b.cancel(item["id"])
        self.assertEqual(self.b.handle(replace(self.base, kind=Kind.DATA, data=b"x"), "dongle").kind, Kind.ERROR)
        self.b.settings["max_mb"] = 1
        oversized = replace(self.base, transfer=124, data=struct.pack("<I", 1024 * 1024 + 1) + b"x" * 32)
        self.assertEqual(self.b.handle(oversized, "dongle").kind, Kind.ERROR)

    def test_preferences_follow_computer_across_dongle_changes(self):
        settings = dict(mode="dongle", name="Laptop", max_mb=32, speed="conservative")
        settings = TransferService.validate_settings(settings)
        self.a.configure(settings)
        self.a.disconnected(); self.a.slot, self.a.session = 3, 303
        self.assertEqual(self.a.settings, settings)
        self.assertEqual(TransferService(self.root / "A").settings, settings)

    def test_real_encrypted_tcp_transfer(self):
        # Incompressible content exercises multiple 64 KiB records and real file staging.
        file = self.root / "binary.dat"; file.write_bytes(secrets.token_bytes(2 * 1024 * 1024))
        build_bundle(self.bundle, {}, [file])
        self.base = replace(self.base, data=struct.pack("<I", self.bundle.stat().st_size) + bytes.fromhex(file_digest(self.bundle)))
        thread = threading.Thread(target=self.b._listen, daemon=True); thread.start()
        self.b.threads.append(thread)
        end = time.monotonic() + 2
        while not self.b.port and time.monotonic() < end:
            time.sleep(.01)
        advert = replace(self.b.advertisement(1, 101), addresses=("127.0.0.1",))
        self.a._network_send(self.base, advert, self.bundle)
        self.assertEqual(next(iter(self.b.items.values()))["state"], "Ready")
        incoming = next(iter(self.b.incoming.values()))
        self.assertEqual((incoming["directory"] / "content/files/binary.dat").read_bytes(), file.read_bytes())

    def _capture(self):
        directory = self.a.root / "outgoing" / "capture"; directory.mkdir()
        (directory / "text").write_bytes("Hello 😀".encode())
        capture = directory / "capture.json"
        capture.write_text(json.dumps(dict(formats={"text/plain": "text"}, files=[])))
        return capture

    def _wait_complete(self, identifier):
        end = time.monotonic() + 5
        while time.monotonic() < end:
            self.a.ui_seen = self.b.ui_seen = time.monotonic()
            item = self.a.items.get(identifier, {})
            if item.get("state") in ("Complete", "Failed"):
                self.assertEqual(item["state"], "Complete", item)
                return
            time.sleep(.01)
        self.fail(f"Transfer timed out: {self.a.items}")

    def test_background_worker_falls_back_from_unreachable_network(self):
        self.a.start(); self.b.start()
        # Discovery is authentic but the advertised endpoint is unreachable.
        original = self.b.advertisement
        with patch.object(self.b, "advertisement", side_effect=lambda *args: replace(original(*args), port=1, addresses=("127.0.0.1",))):
            def post(packet):
                reply = self.b.handle(packet, "dongle")
                if reply:
                    self.a.deliver(reply)
            self.a._post = post
            identifier = self.a.send(2, self._capture())["id"]
            self._wait_complete(identifier)
        self.assertEqual(self.a.items[identifier]["transport"], "dongle")
        self.assertEqual(len(self.b.incoming), 1)

    def test_radio_workers_retry_a_lost_ack_without_duplicate_content(self):
        self.a.settings["mode"] = self.b.settings["mode"] = "dongle"
        self.a.start(); self.b.start()
        dropped = []
        def bridge():
            while not self.a.stop.is_set():
                for source, target in ((self.a, self.b), (self.b, self.a)):
                    try:
                        packet = source._take_radio_packet()
                    except queue.Empty:
                        continue
                    if source is self.b and packet.kind == Kind.ACK and packet.offset and not dropped:
                        dropped.append(packet); continue
                    target.deliver(packet)
                time.sleep(.001)
        thread = threading.Thread(target=bridge, daemon=True); thread.start()
        identifier = self.a.send(2, self._capture())["id"]
        self._wait_complete(identifier)
        self.a.stop.set(); thread.join(1)
        self.assertEqual(len(dropped), 1)
        self.assertEqual(len(self.b.incoming), 1)

    def test_receiver_cancel_stops_the_background_sender(self):
        self.a.settings["mode"] = self.b.settings["mode"] = "dongle"
        self.a.start(); self.b.start()
        def bridge():
            while not self.a.stop.is_set():
                for source, target in ((self.a, self.b), (self.b, self.a)):
                    try:
                        packet = source._take_radio_packet()
                    except queue.Empty:
                        continue
                    # Hold data to model an interrupted link while allowing control traffic.
                    if packet.kind != Kind.DATA:
                        target.deliver(packet)
                time.sleep(.001)
        thread = threading.Thread(target=bridge, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 1)
        self.addCleanup(self.a.stop.set)
        identifier = self.a.send(2, self._capture())["id"]
        deadline = time.monotonic() + 3
        while not self.b.incoming and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertTrue(self.b.incoming)
        incoming = next(iter(self.b.incoming.values()))
        self.b.cancel(incoming["id"])
        while time.monotonic() < deadline:
            item = self.a.items[identifier]
            if item.get("state") == "Cancelled":
                break
            time.sleep(.01)
        self.assertEqual(self.a.items[identifier]["state"], "Cancelled")
        self.assertEqual(self.b.items[incoming["id"]]["state"], "Cancelled")
        self.assertEqual(incoming["offset"], 0)

    def test_network_only_never_falls_back_to_dongle(self):
        self.a.settings["mode"] = "network"
        self.assertEqual(transports(self.a.settings["mode"], self.b.settings["mode"]), ["network"])
        with self.assertRaises(OSError):
            self.a._network_send(self.base, replace(self.b.advertisement(1, 101), addresses=()), self.bundle)
        self.assertFalse(self.b.incoming)

    def test_credentials_are_specific_to_both_computers_and_sessions(self):
        credential = self.a.advertisement(2, 202).token
        self.assertNotEqual(credential, self.a.advertisement(3, 303).token)
        self.assertNotEqual(credential, self.a.advertisement(2, 203).token)
        self.assertNotEqual(credential, self.a.token)

    def test_accepted_text_remains_saveable_without_losing_retry_idempotency(self):
        self.a._stream(self.base, self.bundle, self.request, 160)
        identifier = next(iter(self.b.items))
        self.b.applied(identifier)
        self.assertTrue(next(iter(self.b.incoming.values()))["directory"].exists())
        self.assertEqual(self.b.items[identifier]["state"], "On clipboard")
        saved = Path(self.b.save(identifier, self.root)["path"])
        self.assertEqual(saved.read_text(encoding="utf-8"), "Hello 😀")
        self.assertEqual(self.request(self.base).offset, self.bundle.stat().st_size)
        self.request(replace(self.base, kind=Kind.END, data=b""))
        self.assertEqual(len(self.b.incoming), 1)

    def test_accepted_cache_is_evicted_before_blocking_new_receives(self):
        self.a._stream(self.base, self.bundle, self.request, 160)
        identifier = next(iter(self.b.items))
        self.b.applied(identifier)
        with patch("kms_companion.clipboard_service.MAX_PENDING", 1):
            self.a._stream(replace(self.base, transfer=124), self.bundle, self.request, 160)
        self.assertFalse(self.b.items[identifier]["available"])
        with self.assertRaises(ValueError):
            self.b.content(identifier)
        self.assertEqual(self.request(self.base).offset, self.bundle.stat().st_size)

    def test_automatic_save_has_a_stable_saved_outcome_and_does_not_publish(self):
        timestamp = datetime(2026, 11, 5, 6, 8, 22, 395339)
        original_text = None
        for formats, expected_type, extension in (({"text/plain": b"hello"}, "text", ".txt"),
                ({"text/html": b"<b>rich text</b>", "text/plain": b"rich text"}, "rich", ".html"),
                ({"image/png": b"image"}, "image", ".png")):
            with self.subTest(kind=expected_type):
                build_bundle(self.bundle, formats)
                packet = replace(self.base, transfer=self.base.transfer + len(self.b.incoming),
                    data=struct.pack("<I", self.bundle.stat().st_size) + bytes.fromhex(file_digest(self.bundle)))
                self.a._stream(packet, self.bundle, self.request, 160)
                item = list(self.b.items.values())[-1]
                self.assertEqual(item["content_type"], expected_type)
                with patch("kms_companion.clipboard_content.datetime") as clock:
                    clock.now.return_value = timestamp
                    result = self.b.save(item["id"], self.root, automatic=True)
                saved = Path(result["path"])
                self.assertEqual(saved.suffix, extension)
                if expected_type == "text":
                    original_text = saved
                elif expected_type == "rich":
                    self.assertNotEqual(saved.stem, original_text.stem)
                    self.assertEqual(original_text.read_bytes(), b"hello")
                    self.assertFalse(original_text.with_suffix(".html").exists())
                    self.assertEqual(saved.read_bytes(), b"<b>rich text</b>")
                    self.assertEqual(saved.with_suffix(".txt").read_bytes(), b"rich text")
                self.assertEqual(item["state"], "Saved")
                self.assertFalse(item.get("prepared"))
                self.request(replace(packet, kind=Kind.END, data=b""))
                self.assertEqual(item["state"], "Saved")

    def test_saved_files_prepared_for_paste_are_not_expired_or_evicted(self):
        self._file_bundle()
        self.a._stream(self.base, self.bundle, self.request, 160)
        identifier = next(iter(self.b.items))
        self.b.save(identifier, self.root, automatic=True)
        self.b.content(identifier)
        incoming = next(iter(self.b.incoming.values()))
        incoming["seen"] -= 601
        self.b._expire()
        self.assertTrue(incoming["directory"].exists())
        self.assertIn(incoming["key"], self.b.incoming)
        with patch("kms_companion.clipboard_service.MAX_PENDING", 1):
            response = self.b.handle(replace(self.base, transfer=124), "dongle")
        self.assertEqual(response.kind, Kind.ERROR)
        self.assertTrue(incoming["directory"].exists())


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "Qt UI is optional")
class ClipboardUiTests(unittest.TestCase):
    def test_qt_capture_and_publish_text_rich_image_and_files(self):
        from PySide6.QtCore import QMimeData, QUrl
        from PySide6.QtGui import QImage
        from PySide6.QtWidgets import QApplication
        from kms_companion.clipboard_ui import capture_clipboard, publish_clipboard
        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            file = root / "hello.txt"; file.write_text("file", encoding="utf-8")
            for kind in ("text", "rich", "image", "files"):
                mime = QMimeData()
                if kind in ("text", "rich"):
                    mime.setText("Hello 😀")
                    if kind == "rich": mime.setHtml("<b>Hello 😀</b>")
                elif kind == "image":
                    image = QImage(8, 8, QImage.Format.Format_ARGB32); image.fill(0xff123456); mime.setImageData(image)
                else:
                    mime.setUrls([QUrl.fromLocalFile(str(file))])
                if kind in ("image", "files"):
                    mime.setText("plain-text fallback")
                app.clipboard().setMimeData(mime)
                if kind != "text":
                    self.assertIsNone(capture_clipboard(app.clipboard(), root, 1024 * 1024, automatic_types={"text"}))
                capture = Path(capture_clipboard(app.clipboard(), root, 1024 * 1024, automatic_types={kind}))
                descriptor = json.loads(capture.read_text())
                formats = {m: (capture.parent / n).read_bytes() for m, n in descriptor["formats"].items()}
                bundle = capture.parent / "bundle.zip"
                build_bundle(bundle, formats, descriptor["files"])
                staging = capture.parent / "unpacked"; manifest = unpack_bundle(bundle, staging)
                publish_clipboard(app.clipboard(), dict(directory=str(staging), manifest=manifest))
                self.assertIsNone(capture_clipboard(app.clipboard(), root, 1024 * 1024, automatic_types={kind}))
                received = app.clipboard().mimeData()
                self.assertEqual(manifest["kind"], "text" if kind == "text" else "files" if kind == "files" else "content")
                if kind == "text": self.assertEqual(received.text(), "Hello 😀")
                if kind == "rich": self.assertTrue(received.hasHtml())
                if kind == "image": self.assertTrue(received.hasImage())
                if kind == "files": self.assertEqual(Path(received.urls()[0].toLocalFile()).read_text(), "file")
            app.clipboard().clear()
