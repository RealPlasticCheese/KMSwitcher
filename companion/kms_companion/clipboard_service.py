"""Clipboard transfers. HID stays on its owner; disk and TCP work run off the input loop."""
from dataclasses import dataclass, replace
from contextlib import suppress
import hmac
import json
from pathlib import Path
import queue
import secrets
import shutil
import socket
import struct
import threading
import time
import zipfile
from kms_transport import PROFILES, send_stream
from .clipboard_transport import ClipboardDestination
from .clipboard_content import FORMAT_EXTENSIONS, build_bundle, file_digest, unpack_bundle, save_files, save_content
from .clipboard_preferences import CONTENT_LABELS, content_type, receiving_settings, sending_settings
from .clipboard_protocol import (Advertisement, DATA_BYTES, Kind, LAN_HEADER, MODES, Packet, QUERY_MACHINE_ID,
                                 SecureRecords, local_addresses, network_key, receive_exact, transports)

MAX_PENDING = 8
RECEIVE_TTL = 600
RADIO_WINDOW = 4
RADIO_REPLY_TIMEOUT = .8
RADIO_MAX_BACKOFF = 2
RADIO_STALL_TIMEOUT = 30
RADIO_ACK_LIMIT = 16
RADIO_RECEIVE_BURST = 4
RADIO_RECEIVE_BUDGET = .002
RADIO_CREDIT_BUDGET = .012
INCOMING_BUFFER_BYTES = 64 * 1024


@dataclass(frozen=True)
class AutomaticCopy:
    capture: Path
    generation: int
    session: int
    targets: tuple[tuple[int, int, bytes], ...]


class TransferService:
    def __init__(self, directory):
        self.root = Path(directory) / "clipboard"
        self.root.mkdir(parents=True, exist_ok=True)
        if self.root.stat().st_mode & 0o077:
            try:
                self.root.chmod(0o700)
            except OSError:
                pass
        self.settings_file = Path(directory) / "clipboard.json"
        self.settings = {"mode": "network-preferred", "name": socket.gethostname(), "max_mb": 256, "speed": "conservative"}
        self.settings.update(receiving_settings({}))
        self.settings.update(sending_settings({}))
        try:
            self.settings.update(self.validate_settings(json.loads(self.settings_file.read_text(encoding="utf-8"))))
        except (OSError, ValueError, TypeError):
            pass
        self.stop = threading.Event()
        self.cancelled = set()
        self.lock = threading.RLock()
        self.waiter_lock = threading.Lock()
        self.sequence_lock = threading.Lock()
        self.status_lock = threading.RLock()
        self.ack_lock = threading.Lock()
        self.jobs = queue.Queue(maxsize=4)
        self.send_lock = threading.Lock()
        self.automatic_lock = threading.Lock()
        self.automatic_generation = 0
        self.pending_copy = None
        self.machine_id = b""
        self.radio_out = queue.Queue(maxsize=32)
        self.radio_in = queue.Queue(maxsize=16)
        self.radio_acks = {}
        self.prefer_radio_ack = False
        self.radio_target = None
        self.waiters = {}
        self.peers = {}
        self.incoming = {}
        self.items = {}
        self.slot = self.session = self.port = 0
        self.token = secrets.token_bytes(32)
        self.ui_seen = 0
        self.desktop_ready = False
        self.sequence = secrets.randbits(30) or 1
        self.scan_slot = 1
        self.next_scan = 0
        self.listener = None
        self.connections = threading.BoundedSemaphore(2)
        self.stream_state = {}
        self.network_error = ""
        self.addresses = ()
        self.next_addresses = 0
        self.threads = []
        for folder in ("outgoing", "incoming"):
            path = self.root / folder
            path.mkdir(exist_ok=True)

    @staticmethod
    def validate_settings(settings):
        if not isinstance(settings, dict):
            raise ValueError("Expected clipboard settings")
        mode, name, maximum = settings.get("mode"), settings.get("name"), settings.get("max_mb")
        if (mode not in MODES or not isinstance(name, str) or not name.strip() or len(name.encode()) > 48 or
                type(maximum) is not int or not 1 <= maximum <= 2048):
            raise ValueError("Choose a transport, a computer name up to 48 UTF-8 bytes and a 1–2048 MB size limit")
        speed = settings.get("speed", "conservative")
        if speed not in PROFILES:
            raise ValueError("Choose a valid dongle speed profile")
        return dict(mode=mode, name=name.strip(), max_mb=maximum, speed=speed,
                    **receiving_settings(settings), **sending_settings(settings))

    @property
    def limit(self):
        return self.settings["max_mb"] * 1024 * 1024

    def configure(self, settings):
        settings = self.validate_settings(settings)
        discarded = None
        with self.automatic_lock:
            temp = self.settings_file.with_suffix(".tmp")
            temp.write_text(json.dumps(settings), encoding="utf-8")
            temp.replace(self.settings_file)
            if any(settings[key] != self.settings[key] for key in ("automatic_send", "automatic_targets", "mode")):
                self.automatic_generation += 1
                discarded, self.pending_copy = self.pending_copy, None
            self.settings = settings
        if discarded:
            shutil.rmtree(discarded.capture.parent, ignore_errors=True)
        self.next_scan = 0
        return dict(settings)

    def start(self):
        # Start is called after acquiring the helper's exclusive instance lock.
        identity = self.root / "machine-id"
        if identity.exists():
            self.machine_id = identity.read_bytes()
        if len(self.machine_id) != 16:
            self.machine_id = secrets.token_bytes(16)
            temp = identity.with_suffix(".tmp")
            temp.write_bytes(self.machine_id)
            temp.replace(identity)
        for folder in ("outgoing", "incoming"):
            path = self.root / folder
            shutil.rmtree(path)
            path.mkdir()
        for name, fn in (("receive", self._receive_loop), ("send", self._send_loop), ("network", self._listen)):
            thread = threading.Thread(target=fn, daemon=True, name="kms-clipboard-" + name)
            self.threads.append(thread); thread.start()

    def close(self):
        self.stop.set()
        with self.automatic_lock:
            discarded, self.pending_copy = self.pending_copy, None
        if discarded:
            shutil.rmtree(discarded.capture.parent, ignore_errors=True)
        if self.listener:
            self.listener.close()
        for thread in self.threads:
            thread.join(timeout=1)
        with self.lock:
            for incoming in self.incoming.values():
                with suppress(OSError):
                    self._close_incoming(incoming)

    @staticmethod
    def _close_incoming(incoming):
        stream = incoming.pop("stream", None)
        if stream is not None:
            stream.close()

    def ready(self):
        return bool(not self.stop.is_set() and self.slot and self.session and self.desktop_ready and time.monotonic() - self.ui_seen < 5)

    def _credential(self, peer_slot, peer_session):
        binding = struct.pack("<BIBI", self.slot, self.session, peer_slot, peer_session)
        return hmac.digest(self.token, b"KMSwitcher clipboard peer v1\0" + binding, "sha256")

    def advertisement(self, peer_slot, peer_session, include_machine_id=False):
        network = self.settings["mode"] != "dongle"
        return Advertisement(self.settings["mode"], self.ready(), self.port if network else 0,
                             self._credential(peer_slot, peer_session), self.settings["name"], self.addresses if network else (),
                             self.machine_id if include_machine_id else b"")

    def _peer_query(self, slot, session):
        # Legacy peers understand the original advertisement and ignore the request flag.
        return self._new_packet(Kind.QUERY, slot, session, offset=QUERY_MACHINE_ID,
                                data=self.advertisement(slot, session).encode())

    def _remember_peer(self, packet):
        advert = Advertisement.decode(packet.data)
        previous = self.peers.get(packet.source)
        if packet.kind == Kind.QUERY and previous and previous[0] == packet.source_session:
            advert = replace(advert, machine_id=previous[1].machine_id)
        self.peers[packet.source] = (packet.source_session, advert, time.monotonic())
        return advert

    def _sequence(self):
        with self.sequence_lock:
            self.sequence = (self.sequence + 1) & 0xffffffff or 1
            return self.sequence

    def _new_packet(self, kind, slot, session, **kwargs):
        return Packet(kind, self.slot, slot, self.session, session, self._sequence(), **kwargs)

    def _post(self, packet):
        try:
            self.radio_out.put_nowait(packet)
        except queue.Full:
            pass  # End-to-end requests retry; never block the HID owner.

    def _queue_radio_reply(self, request, reply):
        if request.kind == Kind.DATA and reply.kind == Kind.ACK:
            key = (reply.source_session, reply.target, reply.target_session, reply.transfer)
            with self.ack_lock:
                previous = self.radio_acks.get(key)
                if previous is not None or len(self.radio_acks) < RADIO_ACK_LIMIT:
                    if previous is None or reply.offset >= previous.offset:
                        self.radio_acks[key] = reply
                    return
        self._post(reply)  # Keep BEGIN/END/CANCEL and error replies individually addressable.

    def _take_radio_packet(self):
        with self.ack_lock:
            if self.radio_acks and self.prefer_radio_ack:
                self.prefer_radio_ack = False
                return self.radio_acks.pop(next(iter(self.radio_acks)))
        try:
            packet = self.radio_out.get_nowait()
            self.prefer_radio_ack = True
            return packet
        except queue.Empty:
            with self.ack_lock:
                if not self.radio_acks:
                    raise
                self.prefer_radio_ack = False
                return self.radio_acks.pop(next(iter(self.radio_acks)))

    def pump(self, receiver, status, desktop_ready):
        """Called only by the HID owner; no disk I/O or TCP here."""
        identity = (status["slot"], receiver.session) if status.get("clipboard") and status.get("usb_ready") else (0, 0)
        if identity != (self.slot, self.session):
            self.slot, self.session = identity
            self.token = secrets.token_bytes(32)
            self.peers = {}
            self.next_scan = 0
            with self.ack_lock:
                self.radio_acks.clear()
        self.desktop_ready = desktop_ready
        if not self.slot:
            return
        snapshot = status.get("stream")
        profile = self.settings.get("speed", "conservative")
        credit_aware = snapshot is not None or getattr(receiver, "stream_supported", False) is True
        budget = RADIO_CREDIT_BUDGET if credit_aware else RADIO_RECEIVE_BUDGET
        receive_until = time.monotonic() + budget
        incoming = snapshot["incoming"] if snapshot is not None else RADIO_RECEIVE_BURST
        for _ in range(incoming):
            raw = receiver.clipboard_receive()
            if not raw:
                break
            self.deliver(raw)
            if time.monotonic() >= receive_until:
                break
        if snapshot is not None:
            capacity = receiver.stream_capacity(profile, snapshot)
        else:
            capacity = receiver.stream_capacity(profile) if hasattr(receiver, "stream_capacity") else 1
        self.stream_state = dict(getattr(receiver, "stream_state", {})) if isinstance(getattr(receiver, "stream_state", None), dict) else {}
        send_until = time.monotonic() + budget
        for _ in range(max(1, capacity)):
            try:
                packet = self._take_radio_packet()
                if packet.kind == Kind.DATA and not capacity:
                    with suppress(queue.Full): self.radio_out.put_nowait(packet)
                    break
                if packet.source_session == self.session:
                    receiver.clipboard_send(packet)
            except queue.Empty:
                break
            if time.monotonic() >= send_until:
                break
        now = time.monotonic()
        if now >= self.next_scan:
            if self.radio_target is not None:
                slot, session = self.radio_target
                self._post(self._peer_query(slot, session))
                self.next_scan = now + 1
                return  # Reuse the bound session; never block the USB owner on discovery during a send.
            slot = self.scan_slot
            self.scan_slot = slot % 7 + 1
            self.next_scan = now + .5
            if slot != self.slot:
                try:
                    peer = receiver.clipboard_peer(slot)
                    if peer["session"] and peer["age_ms"] < 3000:
                        self._post(self._peer_query(slot, peer["session"]))
                except (OSError, RuntimeError, ValueError):
                    pass

    def disconnected(self):
        self.slot = self.session = 0
        self.desktop_ready = False
        self.peers = {}
        with self.ack_lock:
            self.radio_acks.clear()

    def deliver(self, packet):
        if packet.target != self.slot or packet.target_session != self.session:
            return
        if packet.kind in (Kind.INFO, Kind.ACK, Kind.ERROR):
            with self.waiter_lock:
                waiter = self.waiters.get((packet.source, packet.source_session, packet.sequence))
            if waiter:
                try:
                    waiter.put_nowait(packet)
                except queue.Full:
                    pass
                return
            if packet.kind in (Kind.ACK, Kind.ERROR):
                return  # Late replies are not requests and must never provoke another reply.
        try:
            self.radio_in.put_nowait(packet)
        except queue.Full:
            pass

    def _radio_request(self, packet):
        if packet.kind not in (Kind.QUERY, Kind.CANCEL) and self.settings["mode"] == "network":
            raise ValueError("This computer permits network transfers only")
        waiter = queue.Queue(maxsize=1)
        key = packet.target, packet.target_session, packet.sequence
        with self.waiter_lock:
            self.waiters[key] = waiter
        try:
            expires = time.monotonic() + RADIO_STALL_TIMEOUT
            retry_at, delay = 0, RADIO_REPLY_TIMEOUT
            while time.monotonic() < expires:
                self._check_active(packet)
                now = time.monotonic()
                if now >= retry_at:
                    self._post(packet)
                    retry_at = now + delay
                    delay = min(RADIO_MAX_BACKOFF, delay * 2)
                try:
                    reply = waiter.get(timeout=max(0, min(.05, retry_at - now, expires - now)))
                    if reply.transfer != packet.transfer:
                        continue
                    self._check_active(packet)
                    if reply.kind == Kind.ERROR:
                        raise ValueError(reply.data.decode("utf-8", errors="replace"))
                    if reply.kind != (Kind.INFO if packet.kind == Kind.QUERY else Kind.ACK):
                        raise ValueError("Unexpected dongle transfer reply")
                    return reply
                except queue.Empty:
                    continue
            raise TimeoutError(f"Destination did not acknowledge the dongle transfer within {RADIO_STALL_TIMEOUT} seconds")
        finally:
            with self.waiter_lock:
                self.waiters.pop(key, None)

    def _check_active(self, packet):
        if self.stop.is_set() or packet.source_session != self.session or packet.source != self.slot:
            raise ConnectionError("Clipboard session changed or companion stopped")
        if packet.transfer in self.cancelled:
            raise ValueError("Clipboard transfer cancelled")
        peer = self.peers.get(packet.target)
        if peer and peer[0] != packet.target_session:
            raise ConnectionError("Destination companion session changed")

    def _item(self, identifier, **values):
        with self.status_lock:
            if identifier not in self.items:
                self.items[identifier] = {"id": identifier, "created_at": time.time()}
            item = self.items[identifier]
            if values.get("state") in ("Sending", "Receiving"):
                item.setdefault("started", time.monotonic())
            item.update(values)
            if "state" in values:
                item["cancellable"] = (item["state"] in ("Preparing", "Sending", "Receiving") or
                                       item["state"] == "Failed" and item.get("direction") == "receive")
            if item.get("started"):
                item["elapsed"] = max(.001, time.monotonic() - item["started"])
                item["bytes_per_second"] = item.get("progress", 0) / item["elapsed"]
            self._trim_history()

    def _trim_history(self):
        # Accepted text/images must not eventually require a tray visit to free slots.
        for identifier, item in list(self.items.items()):
            if len(self.items) < 32:
                break
            if not item.get("prepared") and item.get("state") in ("Complete", "On clipboard", "Saved", "Failed", "Cancelled", "Expired"):
                self.items.pop(identifier, None)

    def status(self, ui=False):
        if ui:
            self.ui_seen = time.monotonic()
        now = time.monotonic()
        peers = []
        # During a dock swap the same computer may briefly appear at both slots.
        # Consumers keyed by machine ID keep the most recently seen entry.
        for slot, (session, advert, seen) in sorted(self.peers.copy().items(), key=lambda entry: entry[1][2]):
            paths = transports(self.settings["mode"], advert.mode)
            if not advert.port or not advert.addresses:
                paths = [p for p in paths if p != "network"]
            available = self.ready() and advert.ready and now - seen < 7 and bool(paths)
            peers.append(dict(slot=slot, session=session, name=advert.name, mode=advert.mode, machine_id=advert.machine_id.hex(),
                              available=available, paths=paths, reason="Ready" if available else
                              "No permitted transport available" if not paths else "Companion unavailable or locked"))
        with self.status_lock:
            items = [dict(i) for i in self.items.values()]
        return dict(settings=dict(self.settings), peers=peers, items=items, supported=bool(self.slot),
                    ready_session=self.session if self.ready() else 0,
                    network_error=self.network_error, stream=dict(self.stream_state))

    def _validated_capture(self, capture):
        capture = Path(capture).resolve()
        outgoing = (self.root / "outgoing").resolve()
        if capture.name != "capture.json" or capture.parent.parent != outgoing or not capture.is_file():
            raise ValueError("Invalid clipboard snapshot")
        return capture

    def send(self, slot, capture):
        with self.lock:
            capture = self._validated_capture(capture)
            try:
                return self._enqueue_send(slot, capture)
            except Exception:
                shutil.rmtree(capture.parent, ignore_errors=True)
                raise

    def _enqueue_send(self, slot, capture):
        if not self.ready():
            raise ValueError("Clipboard sending requires an unlocked desktop and the companion tray")
        peer = self.peers.get(slot)
        if not peer or time.monotonic() - peer[2] >= 7 or not peer[1].ready:
            raise ValueError("Destination companion is unavailable")
        if not transports(self.settings["mode"], peer[1].mode):
            raise ValueError("The computers' transport preferences conflict")
        identifier = self._prepare_send(slot, peer[0], peer[1].name, self.session)
        try:
            self.jobs.put_nowait((identifier, slot, peer[0], capture, self.session))
        except queue.Full as error:
            self._item(identifier, state="Failed", detail="The send queue is full")
            raise ValueError("Four sends are already queued; wait for a transfer to finish") from error
        return {"id": identifier}

    def _prepare_send(self, slot, session, name, own_session, automatic=False):
        with self.status_lock:
            self._trim_history()
            if len(self.items) >= 32:
                raise ValueError("Dismiss older clipboard transfers before sending more")
            identifier = secrets.randbits(32) or 1
            self._item(identifier, direction="send", slot=slot, name=name, state="Preparing", progress=0,
                       source_session=own_session, peer_session=session, automatic=automatic)
        return identifier

    def send_automatic(self, capture, source_session):
        capture = self._validated_capture(capture)
        discarded = None
        accepted = False
        try:
            descriptor = json.loads(capture.read_text(encoding="utf-8"))
            kind = content_type(dict(roots=descriptor["files"], formats=descriptor["formats"]))
            with self.automatic_lock:
                mode = self.settings["automatic_send"][kind]
                if mode == "off" or source_session != self.session or not self.ready():
                    return {"queued": False}
                targets = {}
                for peer in self.status()["peers"]:
                    identifier = peer["machine_id"]
                    if peer["available"] and identifier and (mode == "all" or identifier in self.settings["automatic_targets"]):
                        targets[identifier] = (peer["slot"], peer["session"], bytes.fromhex(identifier))
                self.automatic_generation += 1
                discarded, self.pending_copy = self.pending_copy, None
                if targets:
                    # Move the capture into helper ownership before accepting the IPC request.
                    owned = capture.parent.with_name("auto-" + capture.parent.name)
                    capture.parent.rename(owned)
                    self.pending_copy = AutomaticCopy(owned / capture.name, self.automatic_generation, source_session,
                                                      tuple(targets.values()))
                    accepted = True
            return {"queued": accepted}
        finally:
            if discarded:
                shutil.rmtree(discarded.capture.parent, ignore_errors=True)
            if not accepted:
                shutil.rmtree(capture.parent, ignore_errors=True)

    def cancel(self, identifier):
        with self.lock:
            with self.status_lock:
                item = dict(self.items.get(identifier, {}))
            if not item.get("cancellable"):
                return {"id": identifier}
            self.cancelled.add(identifier)
            self._item(identifier, state="Cancelled", detail="Cancelled locally")
            incoming = next((v for v in self.incoming.values() if v["id"] == identifier), None)
            if incoming:
                with suppress(OSError):
                    self._close_incoming(incoming)
                slot, session, transfer = incoming["key"]
                own_session = incoming["session"]
            else:
                slot, session, transfer = item["slot"], item["peer_session"], identifier
                own_session = item["source_session"]
            if own_session == self.session and self.slot:
                self._post(self._new_packet(Kind.CANCEL, slot, session, transfer=transfer))
        return {"id": identifier}

    def dismiss(self, identifier, release=False):
        with self.lock:
            incoming = next((v for v in self.incoming.values() if v["id"] == identifier), None)
            if incoming and incoming.get("published") and not release:
                raise ValueError("Files prepared for paste remain available until the companion restarts")
            if incoming:
                self._close_incoming(incoming)
                shutil.rmtree(incoming["directory"], ignore_errors=True)
                self.incoming.pop(incoming["key"], None)
        with self.status_lock:
            self.items.pop(identifier, None)
        self.cancelled.discard(identifier)
        return {"id": identifier}

    def clear(self, kind=None):
        if kind is not None and (not isinstance(kind, str) or kind not in CONTENT_LABELS):
            raise ValueError("Choose a valid clipboard type")
        # The sender can hold open files while it waits for a peer.
        if not self.send_lock.acquire(blocking=False):
            raise ValueError("Wait for outgoing clipboard transfers to finish before clearing the list")
        try:
            with self.lock, self.automatic_lock:
                if not self.jobs.empty():
                    raise ValueError("Wait for outgoing clipboard transfers to finish before clearing the list")
                root = self.root.parent.resolve() / self.root.name
                if self.root.resolve() != root:
                    raise ValueError("The clipboard folder points outside its expected location")
                folders = [root / name for name in ("incoming", "outgoing")]
                for folder in folders:
                    if folder.resolve() != folder:
                        raise ValueError("The clipboard staging folder points outside its expected location")
                    folder.mkdir(exist_ok=True)
                if kind is not None:
                    self._clear_type(kind, folders[0])
                    return {}
                self.automatic_generation += 1
                self.pending_copy = None
                for incoming in self.incoming.values():
                    self._close_incoming(incoming)
                self.incoming.clear()
                self.cancelled.clear()
                with self.status_lock:
                    self.items.clear()
                for folder in folders:
                    for path in folder.iterdir():
                        with suppress(FileNotFoundError):
                            if not path.is_symlink() and path.is_dir():
                                if not path.resolve().is_relative_to(folder):
                                    raise ValueError("A clipboard item points outside its staging folder")
                                shutil.rmtree(path)
                            else:
                                path.unlink()
        finally:
            self.send_lock.release()
        return {}

    def _clear_type(self, kind, folder):
        with self.status_lock:
            matching_ids = {identifier for identifier, item in self.items.items()
                            if item.get("content_type", item.get("kind")) == kind}
        for incoming in list(self.incoming.values()):
            manifest = incoming.get("manifest")
            identifier = incoming["id"]
            if identifier not in matching_ids and (not manifest or content_type(manifest) != kind):
                continue
            path = incoming["directory"]
            if path.parent.resolve() != folder or path.resolve() != folder / path.name:
                raise ValueError("A clipboard item points outside its staging folder")
            self._close_incoming(incoming)
            with suppress(FileNotFoundError):
                shutil.rmtree(path)
            self.incoming.pop(incoming["key"], None)
            with self.status_lock:
                self.items.pop(identifier, None)
            self.cancelled.discard(identifier)
        with self.status_lock:
            for identifier in matching_ids:
                self.items.pop(identifier, None)
                self.cancelled.discard(identifier)

    def content(self, identifier, publish=True):
        with self.lock:
            incoming = next((v for v in self.incoming.values() if v["id"] == identifier and v.get("manifest")), None)
            if not incoming or not incoming.get("retained", True):
                raise ValueError("Received clipboard item is not ready")
            if not self.ready() or incoming["session"] != self.session:
                raise ValueError("Unlock the desktop before accepting clipboard content")
            manifest = incoming["manifest"]
            if publish and manifest["kind"] == "files":
                incoming["published"] = True
                self._item(identifier, prepared=True)
            return dict(directory=str(incoming["directory"] / "content"), manifest=manifest, limit=self.limit)

    def inspect(self, identifier):
        with self.lock:
            incoming = next((v for v in self.incoming.values()
                             if v["id"] == identifier and v.get("manifest") and v.get("retained", True)), None)
            if incoming:
                content = self.content(identifier, publish=False)
                directory = Path(content["directory"])
                manifest = content["manifest"]
                if manifest["roots"]:
                    folder = directory / "files"
                    roots = manifest["roots"]
                    if len(roots) == 1:
                        path = directory / roots[0]
                        if path.is_file():
                            return dict(folder=str(folder), path=str(path), file_transfer=True)
                    return dict(folder=str(folder), path=str(folder))
                formats = manifest["formats"]
                entry = next(formats[mime] for mime in FORMAT_EXTENSIONS if mime in formats)
                path = directory / entry
                return dict(folder=str(path.parent), path=str(path))
            with self.status_lock:
                saved_path = self.items.get(identifier, {}).get("saved_path")
            if saved_path:
                return dict(path=saved_path)
            raise ValueError("This clipboard item's content is no longer available")

    def save(self, identifier, parent, automatic=False):
        with self.lock:
            content = self.content(identifier, publish=False)
            if content["manifest"]["roots"]:
                path = save_files(content["directory"], content["manifest"], parent, f"KMSwitcher-{identifier:08x}")
            else:
                path = save_content(content["directory"], content["manifest"], parent)
            self._item(identifier, saved_path=path)
            if automatic:
                self._handled(identifier, "Saved")
            return {"path": path}

    def _handled(self, identifier, outcome):
        incoming = next((v for v in self.incoming.values() if v["id"] == identifier), None)
        if incoming:
            incoming.update(applied=True, outcome=outcome)
            self._item(identifier, state=outcome)

    def applied(self, identifier):
        with self.lock:
            incoming = next((v for v in self.incoming.values() if v["id"] == identifier), None)
            if incoming and incoming.get("manifest", {}).get("kind") != "files":
                self._handled(identifier, "On clipboard")
        return {"id": identifier}

    def _receive_loop(self):
        while not self.stop.is_set():
            try:
                packet = self.radio_in.get(timeout=.2)
                reply = self.handle(packet, "dongle")
                if reply:
                    self._queue_radio_reply(packet, reply)
            except queue.Empty:
                self._expire()
            except Exception as error:
                self.network_error = str(error)

    def _expire(self):
        now = time.monotonic()
        if now >= self.next_addresses:
            self.addresses = local_addresses()
            self.next_addresses = now + 5
        with self.lock:
            for key, incoming in list(self.incoming.items()):
                if (not incoming.get("manifest") or incoming.get("applied") and not incoming.get("published")) and now - incoming["seen"] > RECEIVE_TTL:
                    with suppress(OSError):
                        self._close_incoming(incoming)
                    shutil.rmtree(incoming["directory"], ignore_errors=True)
                    self.incoming.pop(key)
                    self._item(incoming["id"], available=False)
                    if not incoming.get("applied"):
                        self._item(incoming["id"], state="Expired")

    def handle(self, p, transport):
        incoming = None
        try:
            if p.target != self.slot or p.target_session != self.session:
                raise ValueError("Destination computer session changed")
            if p.kind in (Kind.ACK, Kind.ERROR):
                return None
            if p.kind in (Kind.QUERY, Kind.INFO):
                self._remember_peer(p)
                if p.kind == Kind.QUERY:
                    advert = self.advertisement(p.source, p.source_session, p.offset == QUERY_MACHINE_ID)
                    return p.reply(Kind.INFO, advert.encode())
                return None
            if not p.transfer:
                raise ValueError("Missing clipboard transfer identifier")
            key = (p.source, p.source_session, p.transfer)
            with self.lock:
                incoming = self.incoming.get(key)
                peer = self.peers.get(p.source)
                now = time.monotonic()
                established = (incoming is not None and incoming["session"] == p.target_session and
                               now - incoming["seen"] < RECEIVE_TTL)
                with self.status_lock:
                    outgoing = dict(self.items.get(p.transfer, {})) if p.kind == Kind.CANCEL else {}
                bound_outgoing = (outgoing.get("direction") == "send" and
                    (outgoing.get("slot"), outgoing.get("peer_session"), outgoing.get("source_session")) ==
                    (p.source, p.source_session, p.target_session))
                if not peer or peer[0] != p.source_session:
                    raise ValueError("Source companion session changed or is unavailable")
                # Discovery is low priority too. Its age must not invalidate an
                # established, session-bound transfer during input congestion.
                if now - peer[2] >= 7 and not (established or bound_outgoing):
                    raise ValueError("Source companion discovery expired")
                if p.kind != Kind.CANCEL and not self.ready():
                    raise ValueError("Destination companion is locked or unavailable")
                if transport not in transports(self.settings["mode"], peer[1].mode):
                    raise ValueError("Transfer blocked by computer transport preferences")
                if p.kind == Kind.CANCEL:
                    if incoming and not incoming.get("manifest"):
                        self.cancelled.add(incoming["id"])
                        with suppress(OSError):
                            self._close_incoming(incoming)
                        self._item(incoming["id"], state="Cancelled", detail="Cancelled by the sender")
                    if bound_outgoing and outgoing.get("cancellable"):
                        self.cancelled.add(p.transfer)
                        self._item(p.transfer, state="Cancelled", detail="Cancelled by the recipient")
                    return p.reply(Kind.ACK)
                if p.kind == Kind.BEGIN:
                    if len(p.data) != 36:
                        raise ValueError("Invalid clipboard transfer header")
                    size = struct.unpack_from("<I", p.data)[0]
                    digest = p.data[4:].hex()
                    if not 0 < size <= self.limit:
                        raise ValueError("Incoming clipboard exceeds this computer's size limit")
                    if incoming:
                        if (size, digest) != (incoming["size"], incoming["digest"]):
                            raise ValueError("Transfer identifier reused for different content")
                    else:
                        # Keep accepted content saveable, evicting this bounded cache first.
                        pending = [i for i in self.incoming.values() if i.get("retained", True)]
                        for cached in list(pending):
                            if len(pending) < MAX_PENDING and sum(i["size"] for i in pending) + size <= 2 * self.limit:
                                break
                            if cached.get("applied") and not cached.get("published"):
                                shutil.rmtree(cached["directory"], ignore_errors=True)
                                cached["retained"] = False
                                pending.remove(cached)
                                self._item(cached["id"], available=False)
                        if len(pending) >= MAX_PENDING or sum(i["size"] for i in pending) + size > 2 * self.limit:
                            raise ValueError("Dismiss incoming items to free clipboard storage")
                        identifier = secrets.randbits(32) or 1
                        directory = self.root / "incoming" / f"{identifier:08x}"
                        directory.mkdir()
                        incoming = dict(key=key, id=identifier, directory=directory, size=size, digest=digest,
                                        offset=0, seen=time.monotonic(), source=p.source, session=p.target_session)
                        (directory / "bundle.zip").touch()
                        self.incoming[key] = incoming
                elif not incoming:
                    raise ValueError("Begin the clipboard transfer first")
                if incoming["id"] in self.cancelled:
                    raise ValueError("Recipient cancelled the clipboard transfer")
                if incoming["session"] != self.session:
                    raise ValueError("Destination computer changed during transfer")
                incoming["seen"] = time.monotonic()
                if p.kind == Kind.DATA:
                    if p.offset == incoming["offset"] and not incoming.get("manifest"):
                        if not p.data or p.offset + len(p.data) > incoming["size"]:
                            raise ValueError("Invalid clipboard data offset or length")
                        stream = incoming.get("stream")
                        if stream is None:
                            stream = (incoming["directory"] / "bundle.zip").open("r+b", buffering=INCOMING_BUFFER_BYTES)
                            incoming["stream"] = stream
                        if stream.tell() != incoming["offset"]:
                            stream.seek(incoming["offset"])
                        if stream.write(p.data) != len(p.data):
                            raise OSError("Incomplete clipboard staging write")
                        incoming["offset"] += len(p.data)
                elif p.kind == Kind.END and not incoming.get("manifest"):
                    self._close_incoming(incoming)  # Flush before hashing or exposing any content.
                    path = incoming["directory"] / "bundle.zip"
                    if incoming["offset"] != incoming["size"] or file_digest(path) != incoming["digest"]:
                        raise ValueError("Clipboard integrity check failed")
                    content = incoming["directory"] / "content"
                    manifest = unpack_bundle(path, content, self.limit)
                    if not self.ready() or incoming["session"] != self.session:
                        shutil.rmtree(content)
                        raise ValueError("Destination session changed while validating the clipboard")
                    incoming["manifest"] = manifest
                elif p.kind not in (Kind.BEGIN, Kind.DATA, Kind.END):
                    raise ValueError("Unexpected clipboard operation")
                # A successful retry must replace an earlier packet's failure state.
                manifest = incoming.get("manifest")
                state = incoming.get("outcome", "On clipboard") if incoming.get("applied") else "Ready" if manifest else "Receiving"
                self._item(incoming["id"], direction="receive", slot=p.source, name=peer[1].name,
                           state=state, size=incoming["size"], kind=manifest["kind"] if manifest else None,
                           content_type=content_type(manifest) if manifest else None,
                           available=bool(manifest and incoming.get("retained", True)),
                           progress=incoming["offset"], transport=transport, detail="")
                return p.reply(Kind.ACK, offset=incoming["offset"])
        except (OSError, ValueError, KeyError, TypeError, struct.error, zipfile.BadZipFile, RuntimeError) as error:
            with self.lock:
                # Another transport may have committed or dismissed it after the error.
                if (incoming and self.incoming.get(incoming["key"]) is incoming and
                        not incoming.get("manifest") and incoming["id"] not in self.cancelled):
                    self._item(incoming["id"], state="Failed", detail=str(error))
            return p.reply(Kind.ERROR, str(error).encode("utf-8")[:DATA_BYTES])

    def _pack_capture(self, capture, cancelled):
        bundle = capture.parent / "bundle.zip"
        descriptor = json.loads(capture.read_text(encoding="utf-8"))
        formats = {}
        total = 0
        for mime, name in descriptor["formats"].items():
            path = (capture.parent / name).resolve()
            if not path.is_relative_to(capture.parent.resolve()):
                raise ValueError("Invalid clipboard capture")
            total += path.stat().st_size
            if total > self.limit:
                raise ValueError("Clipboard formats exceed this computer's size limit")
            formats[mime] = path.read_bytes()
        manifest = build_bundle(bundle, formats, descriptor["files"], self.limit, cancelled)
        size = bundle.stat().st_size
        return bundle, manifest, size, struct.pack("<I", size) + bytes.fromhex(file_digest(bundle))

    def _send_bundle(self, identifier, slot, session, own_session, packed, machine_id=b"", current=None):
        bundle, manifest, size, metadata = packed
        if own_session != self.session:
            raise ValueError("Source computer session changed")
        peer = self._radio_request(self._peer_query(slot, session))
        advert = self._remember_peer(peer)
        if not advert.ready:
            raise ValueError("Destination companion is locked or unavailable")
        if machine_id and machine_id != advert.machine_id:
            raise ValueError("Destination computer changed")
        if current is not None and not current():
            raise ValueError("Automatic clipboard copy was superseded or the source is unavailable")
        paths = transports(self.settings["mode"], advert.mode)
        if not paths:
            raise ValueError("Transport preferences conflict")
        base = self._new_packet(Kind.BEGIN, slot, session, transfer=identifier, data=metadata)
        self._item(identifier, state="Sending", size=size, kind=manifest["kind"], content_type=content_type(manifest))
        last_error = None
        for transport in paths:
            self._check_active(base)
            try:
                self._item(identifier, transport=transport)
                if transport == "network":
                    self._network_send(base, advert, bundle)
                else:
                    self._radio_stream(base, bundle)
                break
            except (OSError, ValueError, TimeoutError) as error:
                last_error = error
                self._item(identifier, detail=f"{transport}: {error}")
        else:
            raise last_error
        self._item(identifier, state="Complete", progress=size)

    def _send_failed(self, identifier, slot, session, own_session, error, superseded=False):
        state = "Cancelled" if superseded or identifier in self.cancelled else "Failed"
        self._item(identifier, state=state, detail=str(error))
        if own_session == self.session and self.slot:
            self._post(self._new_packet(Kind.CANCEL, slot, session, transfer=identifier))
        self.cancelled.discard(identifier)

    def _send_manual(self, job):
        identifier, slot, session, capture, own_session = job
        try:
            if own_session != self.session:
                raise ValueError("Source computer session changed")
            packed = self._pack_capture(capture, lambda: identifier in self.cancelled or self.stop.is_set())
            self._send_bundle(identifier, slot, session, own_session, packed)
        except Exception as error:
            self._send_failed(identifier, slot, session, own_session, error)
        finally:
            shutil.rmtree(capture.parent, ignore_errors=True)

    def _automatic_current(self, copy):
        return copy.generation == self.automatic_generation and copy.session == self.session and self.ready()

    def _send_automatic_copy(self, copy):
        def current():
            return self._automatic_current(copy)

        packed = None
        try:
            for slot, session, machine_id in copy.targets:
                if not current():
                    break
                peer = self.peers.get(slot)
                if not peer or peer[0] != session or peer[1].machine_id != machine_id:
                    continue
                try:
                    identifier = self._prepare_send(slot, session, peer[1].name, copy.session, automatic=True)
                except ValueError:
                    break  # Preserve the existing transfer history bound.
                try:
                    if packed is None:
                        packed = self._pack_capture(copy.capture, lambda: identifier in self.cancelled or not current())
                    self._send_bundle(identifier, slot, session, copy.session, packed, machine_id, current)
                except Exception as error:
                    self._send_failed(identifier, slot, session, copy.session, error, superseded=not current())
                    if packed is None:
                        break
                yield  # A queued manual transfer can run before the next recipient.
        finally:
            shutil.rmtree(copy.capture.parent, ignore_errors=True)

    def _send_loop(self):
        automatic = None
        try:
            while not self.stop.is_set():
                with self.send_lock:
                    try:
                        job = self.jobs.get_nowait()
                    except queue.Empty:
                        if automatic is None:
                            with self.automatic_lock:
                                copy, self.pending_copy = self.pending_copy, None
                            if copy is not None:
                                automatic = self._send_automatic_copy(copy)
                        if automatic is not None:
                            try:
                                next(automatic)
                            except StopIteration:
                                automatic = None
                            continue
                    else:
                        self._send_manual(job)
                        continue
                self.stop.wait(.1)
        finally:
            if automatic is not None:
                automatic.close()

    def _radio_stream(self, base, bundle):
        self.radio_target = (base.target, base.target_session)
        try:
            self._radio_transfer(base, bundle)
        finally:
            self.radio_target = None

    def _radio_transfer(self, base, bundle):
        self._stream(base, bundle, None)

    def _stream(self, base, bundle, request, chunk=None):
        def check():
            self._check_active(base)
            if request is None and self.settings["mode"] == "network":
                raise ValueError("This computer permits network transfers only")

        with bundle.open("rb") as source:
            send_stream(source, bundle.stat().st_size, ClipboardDestination(self, base, request, chunk),
                        profile=self.settings.get("speed", "conservative"), check=check, clock=time.monotonic,
                        progress=lambda done, total: self._item(base.transfer, progress=done, detail=""),
                        waiting=lambda: self._item(base.transfer, detail="Waiting for dongle traffic; retrying"))

    def _network_send(self, base, advert, bundle):
        header = LAN_HEADER.pack(b"KCL1", base.source, base.target, base.source_session, base.target_session, secrets.token_bytes(16))
        error = ConnectionError("No destination network address is available")
        for address in advert.addresses:
            try:
                with socket.create_connection((address, advert.port), timeout=2) as sock:
                    sock.settimeout(8)
                    sock.sendall(header)
                    handshake = header + receive_exact(sock, 16)
                    key = network_key(advert.token, self._credential(base.target, base.target_session), handshake)
                    records = SecureRecords(sock, key, handshake, True)
                    def request(packet):
                        self._check_active(base)
                        if self.settings["mode"] == "dongle":
                            raise ValueError("This computer now permits dongle transfers only")
                        records.send(packet)
                        response = records.receive()
                        if (response.source, response.target, response.source_session, response.target_session,
                            response.sequence, response.transfer) != (packet.target, packet.source, packet.target_session,
                            packet.source_session, packet.sequence, packet.transfer):
                            raise ValueError("Network reply does not match the clipboard request")
                        if response.kind != Kind.ACK:
                            raise ValueError(response.data.decode("utf-8", errors="replace"))
                        return response
                    self._stream(base, bundle, request)
                    return
            except (OSError, ValueError) as failure:
                error = failure
        raise error

    def _listen(self):
        while not self.stop.is_set():
            if self.settings["mode"] == "dongle":
                self.stop.wait(.5); continue
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                    listener.bind(("0.0.0.0", 0)); listener.listen(2); listener.settimeout(.5)
                    self.listener = listener; self.port = listener.getsockname()[1]; self.network_error = ""
                    while not self.stop.is_set() and self.settings["mode"] != "dongle":
                        try:
                            connection, _ = listener.accept()
                        except socket.timeout:
                            continue
                        if self.connections.acquire(blocking=False):
                            threading.Thread(target=self._serve_connection, args=(connection,), daemon=True).start()
                        else:
                            connection.close()
            except OSError as error:
                if not self.stop.is_set():
                    self.network_error = str(error)
                    self.stop.wait(1)
            finally:
                self.port = 0; self.listener = None

    def _serve_connection(self, connection):
        try:
            with connection:
                connection.settimeout(8)
                header = receive_exact(connection, LAN_HEADER.size)
                magic, source, target, source_session, target_session, _ = LAN_HEADER.unpack(header)
                peer = self.peers.get(source)
                if (magic != b"KCL1" or target != self.slot or target_session != self.session or not self.ready() or
                        not peer or peer[0] != source_session or time.monotonic() - peer[2] > 7 or
                        "network" not in transports(self.settings["mode"], peer[1].mode)):
                    return
                challenge = secrets.token_bytes(16)
                connection.sendall(challenge)
                handshake = header + challenge
                key = network_key(self._credential(source, source_session), peer[1].token, handshake)
                records = SecureRecords(connection, key, handshake, False)
                while not self.stop.is_set():
                    packet = records.receive()
                    if (packet.source, packet.target, packet.source_session, packet.target_session) != (
                            source, target, source_session, target_session):
                        return
                    response = self.handle(packet, "network")
                    if not response:
                        return
                    records.send(response)
                    if packet.kind in (Kind.END, Kind.CANCEL) or response.kind == Kind.ERROR:
                        return
        except Exception:
            pass  # Unauthenticated or abandoned connections never change the local clipboard.
        finally:
            self.connections.release()
