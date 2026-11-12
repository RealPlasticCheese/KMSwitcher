"""Inspect, archive, clear and update devices through the master's UART.

Uses the pinned environment's pyserial. Opening the port leaves DTR/RTS low.
The serial and radio legs share the same CRC-protected management envelope.
"""
import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import re
import secrets
import struct
import sys
import time
import zlib

from kms_transport.diagnostics import IDENTITY, METRICS, EVENTS, decode_identity, decode_summary, decode_radio

HEADER = struct.Struct('<4sBBBBB3xIIIIH')
MAX_DATA = 192
REPLY, SUMMARY, FORCE = 1, 2, 4
INFO, STATS, HISTORY_BEGIN, HISTORY_READ, CLEAR, BEGIN, WRITE, END, ACTIVATE, ABORT = range(1, 11)
CACHE_INFO, CACHE_BEGIN, CACHE_WRITE, CACHE_END, CACHE_ABORT, CACHE_INSTALL, CACHE_PROGRESS = range(13, 20)
RESTART, CAP_RESTART, RESTART_KEY = 20, 16, 0x52535431
RADIO_STATUS, CAP_RADIO_DIAGNOSTICS = 21, 32
STREAM_STATUS, CAP_STREAM = 22, 64
OK, BAD_REQUEST, BUSY, WRONG_BOOT, OFFSET, INTEGRITY, STORAGE, UNSUPPORTED, CURRENT, EXPIRED, INCOMPATIBLE, TIMEOUT = range(12)
RESULTS = ('ok', 'bad request', 'busy', 'receiver restarted', 'unexpected offset', 'integrity failure',
           'storage failure', 'unsupported', 'already current', 'expired session', 'incompatible image',
           'timeout/unreachable')


class ManagementError(Exception):
    def __init__(self, code, message=None):
        self.code = code
        super().__init__(message or RESULTS[code])


@dataclass
class Frame:
    op: int
    slot: int
    request_id: int
    sender: int = 0
    target: int = 0
    offset: int = 0
    flags: int = 0
    status: int = 0
    data: bytes = b''

    def encode(self):
        if not 1 <= self.slot <= 7 or not 1 <= self.op <= STREAM_STATUS or not self.request_id:
            raise ValueError('Invalid management address or request ID')
        if len(self.data) > MAX_DATA or self.flags & ~7 or not 0 <= self.status <= TIMEOUT:
            raise ValueError('Invalid management payload')
        body = HEADER.pack(b'KMSM', 1, self.op, self.flags, self.status, self.slot,
                           self.request_id, self.sender, self.target, self.offset, len(self.data)) + self.data
        return body + struct.pack('<I', zlib.crc32(body))

    @classmethod
    def decode(cls, wire):
        if not HEADER.size + 4 <= len(wire) <= HEADER.size + MAX_DATA + 4:
            raise ValueError('Invalid management length')
        magic, version, op, flags, status, slot, request_id, sender, target, offset, size = HEADER.unpack_from(wire)
        if magic != b'KMSM' or version != 1 or len(wire) != HEADER.size + size + 4 or wire[9:12] != b'\0\0\0':
            raise ValueError('Invalid management header')
        if zlib.crc32(wire[:-4]) != struct.unpack_from('<I', wire, len(wire) - 4)[0]:
            raise ValueError('Management CRC mismatch')
        frame = cls(op, slot, request_id, sender, target, offset, flags, status, wire[HEADER.size:-4])
        frame.encode()  # Apply the same bounds to received fields.
        return frame


class Controller:
    def __init__(self, serial_port, timeout=6.0, attempts=3, clock=time.monotonic, write_attempts=8, speed_profile="conservative"):
        self.serial = serial_port
        self.timeout = timeout
        self.attempts = attempts
        self.write_attempts = write_attempts
        self.clock = clock
        self.sequence = secrets.randbits(31) or 1
        self.boots = {}
        self.partial = bytearray()
        self.capabilities = {}
        self.speed_profile = speed_profile

    def command(self, text):
        self.serial.write((text + '\n').encode('ascii'))
        self.serial.flush()

    def lines_until(self, deadline):
        while self.clock() < deadline:
            self.partial.extend(self.serial.readline(2048))
            if len(self.partial) > 16384:
                self.partial.clear()
            while b'\n' in self.partial:
                line, _, rest = self.partial.partition(b'\n')
                self.partial[:] = rest
                yield bytes(line).decode('ascii', errors='replace').strip()

    def request(self, slot, op, data=b'', offset=0, force=False, discover=False, allow=(), attempts=None):
        self.sequence = (self.sequence + 1) & 0xffffffff or 1
        local = slot == 0
        request = Frame(op, 1 if local else slot, self.sequence, target=0 if discover else self.boots.get(slot, 0),
                        offset=offset, flags=FORCE if force else 0, data=data)
        command = f'master {request.encode().hex()}' if local else f'remote {slot} {request.encode().hex()}'
        # Keep write retries within the receiver inactivity window at the default timeout.
        attempts = attempts or (max(self.attempts, self.write_attempts) if op in (WRITE, CACHE_WRITE) else self.attempts)
        for _ in range(attempts):
            self.command(command)
            for line in self.lines_until(self.clock() + self.timeout):
                match = re.fullmatch(r'KMSMASTER ([0-9a-fA-F]+)' if local else r'KMSM (\d+) ([0-9a-fA-F]+)', line)
                if not match or (not local and int(match[1]) != slot):
                    continue
                try:
                    response = Frame.decode(bytes.fromhex(match[1 if local else 2]))
                except ValueError:
                    continue
                if (response.request_id, response.slot, response.op, response.offset) != (
                        request.request_id, request.slot, op, offset) or not response.flags & REPLY or response.flags & SUMMARY:
                    continue
                if request.target and response.sender != request.target and response.status != WRONG_BOOT:
                    continue
                if response.status == TIMEOUT and TIMEOUT not in allow:
                    break  # The slave may still be completing a flash erase/verify.
                if response.status not in (OK, *allow):
                    raise ManagementError(response.status)
                return response
        raise ManagementError(TIMEOUT,
                              f'Upload to receiver {slot} timed out at byte {offset}; retry the update'
                              if op in (WRITE, CACHE_WRITE) else None)

    def identity(self, slot):
        response = self.request(slot, INFO, discover=True)
        identity = decode_identity(response.data)
        self.boots[slot] = response.sender
        self.capabilities[slot] = identity['capabilities']
        identity['boot'] = f'{response.sender:08x}'
        return identity

    def configure_stream(self, slot, profile):
        from kms_transport import PROFILES
        if 0 not in self.capabilities:
            self.identity(0)
        if not self.capabilities[0] & CAP_STREAM or not self.capabilities.get(slot, 0) & CAP_STREAM:
            return False
        value = bytes([PROFILES.index(profile)])
        self.request(0, STREAM_STATUS, value)
        if slot:
            self.request(slot, STREAM_STATUS, value)
        return True

    def peers(self):
        self.command('slaves')
        peers, selected = [], 0
        for line in self.lines_until(self.clock() + self.timeout):
            match = re.search(r'KMSPEER (\d+) ([0-9a-fA-F]{8})', line)
            if match:
                slot = int(match[1])
                if 1 <= slot <= 7 and slot not in peers:
                    peers.append(slot)
            route = re.search(r'KMSROUTE CURRENT (\d+)', line)
            if route:
                selected = int(route[1])
            if 'KMSPEERS END' in line:
                return peers, selected
        raise ManagementError(TIMEOUT, 'Master did not return its paired-dongle list')

    def radio_status(self, slot, identity=None):
        identity = identity or self.identity(slot)
        if not identity['capabilities'] & CAP_RADIO_DIAGNOSTICS: return None
        radio = decode_radio(self.request(slot, RADIO_STATUS).data)
        if radio['boot'] != identity['boot']: raise ValueError('Radio status boot does not match identity')
        return radio

    def restart(self, slot, *, before_restart=lambda: None, timeout=20):
        if type(slot) is not int or not 1 <= slot <= 7:
            raise ValueError('Choose a receiver from 1 to 7')
        if not self.identity(0)['capabilities'] & CAP_RESTART:
            raise ManagementError(UNSUPPORTED, 'Update the S3 firmware to restart dongles through the controller')
        before = self.identity(slot)
        if not before['capabilities'] & CAP_RESTART:
            raise ManagementError(UNSUPPORTED, f'Dongle {slot} needs updated receiver firmware for remote restart')
        before_restart()
        # An acknowledgement can be lost while the receiver is already rebooting.
        # Never submit a new restart command: verify a new boot with fresh INFO.
        try:
            self.request(slot, RESTART, struct.pack('<I', RESTART_KEY), attempts=1)
        except ManagementError as error:
            if error.code not in (TIMEOUT, WRONG_BOOT): raise
        until = self.clock() + timeout
        while self.clock() < until:
            try:
                response = self.request(slot, INFO, discover=True, attempts=1)
                after = decode_identity(response.data)
                self.boots[slot] = response.sender
                after['boot'] = f'{response.sender:08x}'
                if after['boot'] != before['boot'] and after['confirmed']:
                    if after['sha256'] != before['sha256']:
                        raise ValueError(f'Dongle {slot} returned with different firmware after restarting')
                    return dict(slot=slot, result='restarted', before=before['boot'], after=after['boot'])
            except ManagementError as error:
                if error.code not in (TIMEOUT, WRONG_BOOT): raise
            time.sleep(.2)
        raise ManagementError(TIMEOUT, f'Dongle {slot} did not return with a new confirmed boot after restarting')

    def route(self, pause):
        self.command('pause' if pause else 'resume')
        marker = 'KMSROUTE PAUSED' if pause else 'KMSROUTE RESUMED'
        for line in self.lines_until(self.clock() + self.timeout):
            if marker in line:
                return
            if 'KMSROUTE ERROR' in line:
                raise ManagementError(BUSY, 'Previous route is unavailable; use select N when it returns')
        raise ManagementError(TIMEOUT, 'Master did not acknowledge routing control')

    def select(self, slot):
        self.command(f'select {slot}')
        for line in self.lines_until(self.clock() + self.timeout):
            if line == f'KMSROUTE SELECTED {slot}':
                return
            if line.startswith('KMSROUTE ERROR'):
                raise ManagementError(BUSY, 'Previous destination is unavailable; select it when it returns')
        raise ManagementError(TIMEOUT, 'Master did not acknowledge destination restoration')

    def history(self, slot):
        self.identity(slot)
        begin = self.request(slot, HISTORY_BEGIN)
        if len(begin.data) != 12:
            raise ValueError('Invalid history snapshot header')
        token, size, crc = struct.unpack('<III', begin.data)
        if not 0 < size <= 98304:
            raise ValueError('History snapshot exceeds the bounded transfer capacity')
        result = bytearray()
        while len(result) < size:
            response = self.request(slot, HISTORY_READ, struct.pack('<I', token), offset=len(result))
            if not response.data or len(response.data) > size - len(result):
                raise ValueError('Invalid history chunk length')
            result.extend(response.data)
        if zlib.crc32(result) != crc:
            raise ManagementError(INTEGRITY, 'Downloaded history checksum mismatch')
        try:
            self.request(slot, HISTORY_READ, struct.pack('<I', token), offset=size)
        except (ManagementError, OSError):
            pass  # Older firmware expires snapshots instead of accepting EOF release.
        return bytes(result)


def restart_targets(controller, targets, emit=lambda row: None):
    peers, selected = controller.peers()
    if not targets or any(type(slot) is not int or slot not in peers for slot in targets):
        raise ValueError('Choose at least one paired receiver')
    ordered = list(dict.fromkeys(slot for slot in targets if slot != selected))
    if selected in targets: ordered.append(selected)
    results, warnings, paused = [], [], False
    def pause_selected(slot):
        nonlocal paused
        if controller.peers()[1] == slot:
            paused = True
            controller.route(True)
    try:
        for slot in ordered:
            emit(dict(slot=slot, result='restarting'))
            try:
                row = controller.restart(slot, before_restart=lambda: pause_selected(slot))
            except ManagementError as error:
                row = dict(slot=slot, result='unreachable' if error.code == TIMEOUT else
                           'unsupported' if error.code == UNSUPPORTED else 'failed', detail=str(error))
            except (OSError, ValueError, RuntimeError) as error:
                row = dict(slot=slot, result='failed', detail=str(error))
            results.append(row); emit(row)
    finally:
        if paused:
            try: controller.route(False)
            except (OSError, ValueError, RuntimeError, ManagementError) as error:
                warnings.append(f'Could not restore input after restarting: {error}')
    return dict(results=results, warnings=warnings,
                success=all(row['result'] == 'restarted' for row in results) and not warnings)


def load_firmware(path, role='receiver'):
    image = path.read_bytes()
    metadata = json.loads(path.with_suffix('.json').read_text(encoding='utf-8'))
    chip, capacity = {'controller': (9, 0x300000), 'receiver': (2, 0x1e0000)}[role]
    if (not isinstance(metadata, dict) or metadata.get('schema') != 1 or metadata.get('role') != role or metadata.get('chip') != chip or
            metadata.get('management_protocol') != 1 or metadata.get('input_protocol') != 2 or
            metadata.get('ota_schema') != 1):
        raise ValueError(f'A {role} firmware image and its generated metadata are required')
    if (len(image) < 56 or len(image) > capacity or image[0] != 0xe9 or
            int.from_bytes(image[12:14], 'little') != chip):
        raise ValueError(f'Image is not compatible with the {role} OTA slots')
    digest = hashlib.sha256(image).hexdigest()
    if metadata.get('size') != len(image) or metadata.get('sha256') != digest:
        raise ManagementError(INTEGRITY, 'Firmware file does not match its generated metadata')
    if image[23] != 1 or hashlib.sha256(image[:-32]).digest() != image[-32:]:
        raise ManagementError(INTEGRITY, 'ESP image appended SHA-256 is missing or invalid')
    if role == 'controller':
        from .cache import bundled_receiver
        bundled_receiver(metadata)  # Check the entire bundled payload before opening a port.
    return image, metadata


def cache_identity(controller):
    master = controller.identity(0)
    if not master['capabilities'] & 8:
        raise ManagementError(UNSUPPORTED, 'S3 receiver cache needs the one-time wired partition-table installation')
    response = controller.request(0, CACHE_INFO, allow=(EXPIRED,))
    return None if response.status == EXPIRED else decode_identity(response.data)


def stage_cache(controller, image, metadata, progress=lambda done, total: None):
    from .cache import encode_identity, validate_receiver
    validate_receiver(image, metadata)
    current = cache_identity(controller)
    if current and current['sha256'] == metadata['sha256']:
        return 'skipped'
    token, started = secrets.randbits(32) or 1, False
    try:
        response = controller.request(0, CACHE_BEGIN, struct.pack('<I', token) + encode_identity(metadata), allow=(CURRENT,))
        if response.status == CURRENT:
            return 'skipped'
        started = True
        if response.data != struct.pack('<I', 0):
            raise ManagementError(OFFSET)
        from .stream import upload
        upload(controller, 0, CACHE_WRITE, token, io.BytesIO(image), len(image), progress)
        controller.request(0, CACHE_END, struct.pack('<I', token))
        current = cache_identity(controller)
        if not current or current['sha256'] != metadata['sha256']:
            raise ManagementError(INTEGRITY, 'Cached firmware did not verify')
        return 'updated'
    except BaseException:
        if started:
            try:
                controller.request(0, CACHE_ABORT, struct.pack('<I', token))
            except (ManagementError, OSError):
                pass
        raise


def update_cached_one(controller, slot, metadata, force=False, before_write=lambda: None,
                      progress=lambda done, total: None, sleep=time.sleep):
    before = controller.identity(slot)
    if before['sha256'] == metadata['sha256'] and not force:
        return 'skipped'
    before_write()
    if isinstance(controller, Controller):
        controller.configure_stream(slot, controller.speed_profile)
    response = controller.request(0, CACHE_INSTALL, struct.pack('<I', slot) + bytes.fromhex(metadata['sha256']), force=force)
    if len(response.data) != 4:
        raise ValueError('Invalid cached-update token')
    token, = struct.unpack('<I', response.data)
    last, deadline = None, controller.clock() + 120
    while controller.clock() < deadline:
        sleep(0.25)
        response = controller.request(0, CACHE_PROGRESS, struct.pack('<I', token))
        if len(response.data) != 24:
            raise ValueError('Invalid cached-update progress')
        actual, state, result, offset, total, target = struct.unpack('<6I', response.data)
        if actual != token or target != slot or total != metadata['size'] or offset > total:
            raise ValueError('Cached-update progress belongs to another transfer')
        if last != (state, offset):
            deadline, last = controller.clock() + 120, (state, offset)
        progress(offset, total)
        if state in (7, 8):
            return 'updated' if state == 7 else 'skipped'
        if state == 9:
            raise ManagementError(result, 'Cached receiver update failed: ' + RESULTS[result])
    raise ManagementError(TIMEOUT, 'Cached receiver update stopped making progress')


def update_one(controller, slot, image, metadata, force=False, before_write=lambda: None,
               progress=lambda done, total: None, sleep=time.sleep):
    before = controller.identity(slot)
    chip = 9 if slot == 0 else 2
    if before['chip'] != chip or not before['capabilities'] & 2:
        raise ManagementError(UNSUPPORTED, 'Device needs the one-time wired OTA migration')
    if metadata['chip'] != chip:
        raise ValueError('Firmware role does not match the selected device')
    if before['sha256'] == metadata['sha256'] and not force:
        return 'skipped'
    if not before['confirmed']:
        raise ManagementError(BUSY, 'Receiver has not confirmed its current firmware')
    before_write()
    token = secrets.randbits(32) or 1
    begin = struct.pack('<II32sI', token, len(image), bytes.fromhex(metadata['sha256']), chip)
    started, activating = False, False
    try:
        # A released receiver may still be waiting for its previous input lease.
        for attempt in range(8):
            response = controller.request(slot, BEGIN, begin, force=force, allow=(CURRENT, BUSY))
            if response.status != BUSY:
                break
            sleep(0.5)
        if response.status == CURRENT:
            return 'skipped'
        if response.status == BUSY:
            raise ManagementError(BUSY)
        started = True
        if response.data != struct.pack('<I', 0):
            raise ValueError('Unexpected initial upload offset')
        from .stream import upload
        upload(controller, slot, WRITE, token, io.BytesIO(image), len(image), progress)
        controller.request(slot, END, struct.pack('<I', token))
        activating = True
        try:
            if slot == 0:
                controller.request(slot, ACTIVATE, struct.pack('<I', token), attempts=1)
            else:
                controller.request(slot, ACTIVATE, struct.pack('<I', token))
        except ManagementError as error:
            if error.code not in (TIMEOUT, WRONG_BOOT):
                activating = False
                raise
        # Activation acknowledgements can be lost during reboot. Identity and
        # application confirmation, rather than the acknowledgement, decide success.
        deadline = controller.clock() + 60
        while controller.clock() < deadline:
            sleep(0.5)
            try:
                after = controller.identity(slot)
            except ManagementError as error:
                if error.code not in (TIMEOUT, WRONG_BOOT):
                    raise
                continue
            if (after['boot'] != before['boot'] and after['sha256'] == metadata['sha256'] and
                    after['confirmed']):
                return 'updated'
            if (metadata['sha256'] != before['sha256'] and after['boot'] != before['boot'] and
                    after['sha256'] == before['sha256']):
                raise ManagementError(INTEGRITY, 'Receiver rolled back to its previous firmware')
        raise ManagementError(TIMEOUT, 'Expected firmware was not confirmed after reboot')
    except BaseException:
        if started and not activating:
            try:
                controller.request(slot, ABORT, struct.pack('<I', token))
            except ManagementError:
                pass  # The receiver independently abandons inactive transfers.
        raise


def update_targets(controller, targets, selected, image, metadata, force=False, emit=print, *, cached=False):
    results, paused = [], False
    # Preserve normal input while inactive receivers update; update the selected
    # one last and pause its route only if its image actually needs writing.
    ordered = [slot for slot in targets if slot != selected] + ([selected] if selected in targets else [])
    try:
        for slot in ordered:
            def pause_selected():
                nonlocal paused
                if slot == selected and not paused:
                    paused = True
                    controller.route(True)
            try:
                last_percent = [-1]
                def progress(done, total):
                    percent = done * 100 // total
                    if percent // 5 != last_percent[0] // 5:
                        emit(f'Slave {slot}: {percent}% ({done}/{total} bytes)')
                    last_percent[0] = percent
                outcome = (update_cached_one(controller, slot, metadata, force, pause_selected, progress) if cached else
                           update_one(controller, slot, image, metadata, force, pause_selected, progress))
                record = dict(slot=slot, result=outcome)
            except (ManagementError, ValueError) as error:
                record = dict(slot=slot, result='unreachable' if isinstance(error, ManagementError) and
                              error.code == TIMEOUT else 'failed', detail=str(error))
            results.append(record)
            emit(json.dumps(record))
    finally:
        if paused:
            try:
                controller.route(False)
            except (ManagementError, OSError) as error:
                emit(f'Could not confirm route restoration: {error}')
    return results


def update_master(controller, image, metadata, force=False, emit=print):
    """Application update at 115200 baud; restore the exact previous destination."""
    _, selected = controller.peers()
    paused = False
    result = dict(slot=0, result='failed', warnings=[])
    def pause():
        nonlocal paused
        paused = True
        controller.route(True)
    try:
        result['result'] = update_one(controller, 0, image, metadata, force, pause,
            lambda done, total: emit(f'Controller: {done * 100 // total}%'))
        from .cache import bundled_receiver
        receiver = bundled_receiver(metadata)
        if receiver:
            result['receiver_cache'] = stage_cache(controller, *receiver,
                progress=lambda done, total: emit(f'Receiver cache: {done * 100 // total}%'))
    except (ManagementError, ValueError, OSError) as error:
        if result['result'] in ('updated', 'skipped'):
            result['receiver_cache'] = 'failed'
            result['warnings'].append(f'Controller application checked, but receiver cache could not be refreshed: {error}')
        else:
            result['detail'] = str(error)
    finally:
        if paused:
            try:
                if selected:
                    controller.select(selected)
                else:
                    controller.route(True)
            except (ManagementError, OSError) as error:
                result['warnings'].append(f'Could not restore previous destination: {error}')
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--port', help='Master UART port; auto-discover one CH343 bridge if omitted')
    result.add_argument('--baud', type=int, default=115200)
    result.add_argument('--speed', choices=('conservative', 'balanced', 'fast'), default='conservative')
    result.add_argument('--timeout', type=float, default=6)
    sub = result.add_subparsers(dest='operation', required=True)
    sub.add_parser('list')
    sub.add_parser('cache-status')
    restart = sub.add_parser('restart')
    target = restart.add_mutually_exclusive_group(required=True)
    target.add_argument('--slot', type=int, choices=range(1, 8))
    target.add_argument('--all', action='store_true')
    cache_put = sub.add_parser('cache-put')
    cache_put.add_argument('--firmware', type=Path, required=True)
    for name in ('status', 'history', 'clear'):
        command = sub.add_parser(name)
        command.add_argument('--slot', type=int, choices=range(1, 8), required=True)
        if name == 'history':
            command.add_argument('--output', type=Path, required=True)
    command = sub.add_parser('update')
    target = command.add_mutually_exclusive_group(required=True)
    target.add_argument('--slot', type=int, choices=range(1, 8))
    target.add_argument('--all', action='store_true')
    source = command.add_mutually_exclusive_group(required=True)
    source.add_argument('--firmware', type=Path)
    source.add_argument('--cached', action='store_true', help='Install the verified image already held on the S3')
    command.add_argument('--force', action='store_true')
    command.add_argument('--report', type=Path, help='Save per-dongle update outcomes as JSON')
    return result


def main():
    args = parser().parse_args()
    if args.timeout <= 0 or args.baud <= 0:
        raise ValueError('Timeout and baud rate must be positive')
    firmware = load_firmware(args.firmware) if args.operation in ('update', 'cache-put') and getattr(args, 'firmware', None) else None
    import serial
    from serial.tools import list_ports
    port = args.port
    if not port:
        ports = [p.device for p in list_ports.comports() if p.vid == 0x1a86 and p.pid == 0x55d3]
        if len(ports) != 1:
            raise ValueError(f'Specify --port; found {len(ports)} matching master UART bridges')
        port = ports[0]
    connection = serial.Serial()
    connection.port, connection.baudrate = port, args.baud
    connection.timeout, connection.write_timeout = 0.1, 5
    connection.dtr = connection.rts = False
    connection.open()
    try:
        controller = Controller(connection, args.timeout, speed_profile=args.speed)
        if args.operation == 'cache-status':
            print(json.dumps(cache_identity(controller), indent=2))
        elif args.operation == 'cache-put':
            print(stage_cache(controller, *firmware, progress=lambda done, total: print(f'Cache: {done * 100 // total}%')))
        elif args.operation == 'list':
            peers, selected = controller.peers()
            print(json.dumps(dict(paired_slots=peers, selected=selected), indent=2))
        elif args.operation == 'restart':
            targets = controller.peers()[0] if args.all else [args.slot]
            report = restart_targets(controller, targets)
            print(json.dumps(report, indent=2))
            return 0 if report['success'] else 1
        elif args.operation == 'status':
            identity = controller.identity(args.slot)
            summary = decode_summary(controller.request(args.slot, STATS).data)
            result = dict(identity=identity, diagnostics=summary)
            try:
                radio = controller.radio_status(args.slot, identity)
                if radio is not None: result['radio'] = radio
            except (ManagementError, OSError, ValueError) as error: result['radio_error'] = str(error)
            print(json.dumps(result, indent=2))
        elif args.operation == 'history':
            data = controller.history(args.slot)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(data)
            print(f'Saved verified slave {args.slot} history: {args.output} ({len(data)} bytes)')
        elif args.operation == 'clear':
            controller.identity(args.slot)
            controller.request(args.slot, CLEAR)
            print(f'Slave {args.slot} history cleared and saved; live counters and pairings retained')
        else:
            peers, selected = controller.peers()
            targets = peers if args.all else [args.slot]
            if not targets:
                raise ValueError('No paired dongles to update')
            if args.slot and args.slot not in peers:
                raise ValueError(f'Slave {args.slot} is not paired')
            cached = args.cached
            if cached:
                metadata = cache_identity(controller)
                if not metadata:
                    raise ValueError('S3 receiver cache is empty; upload firmware with cache-put first')
                firmware = (b'', metadata)
            elif controller.identity(0)['capabilities'] & 8:
                stage_cache(controller, *firmware)
                cached = True
            outcomes = update_targets(controller, targets, selected, *firmware, args.force, cached=cached)
            if args.report:
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_text(json.dumps(dict(utc=datetime.now(timezone.utc).isoformat(),
                    firmware=firmware[1], outcomes=outcomes), indent=2) + '\n', encoding='utf-8')
            return int(any(row['result'] in ('failed', 'unreachable') for row in outcomes))
    finally:
        connection.close()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ManagementError, ValueError, OSError) as error:
        print(f'Error: {error}', file=sys.stderr)
        sys.exit(1)
