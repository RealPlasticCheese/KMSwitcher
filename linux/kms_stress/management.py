"""Read-only management load and an oracle for lingering update guards."""
import secrets
import struct
import zlib


HEADER = struct.Struct('<4sBBBBB3xIIIIH')


def management_request(channel, slot, op, boot=0, data=b'', service=None):
    if op not in (1, 13, 19, 21): raise ValueError('Stress inspections are read-only management operations')
    request_id = secrets.randbits(32) or 1
    address = slot or 1
    body = HEADER.pack(b'KMSM', 1, op, 0, 0, address, request_id, 0, boot, 0, len(data)) + data
    wire = body + struct.pack('<I', zlib.crc32(body))
    command = 'master ' if not slot else f'remote {slot} '
    prefix = 'KMSMASTER ' if not slot else f'KMSM {slot} '
    lines = channel.request(command + wire.hex(), prefix, service=service)
    try:
        reply = bytes.fromhex(lines[-1].split()[-1])
        magic, version, operation, flags, status, receiver, ident, sender, target, offset, size = HEADER.unpack_from(reply)
        valid = (magic == b'KMSM' and version == 1 and operation == op and flags == 1 and
                 receiver == address and ident == request_id and sender and (not boot or boot == sender) and
                 (bool(target) if slot else not target) and not offset and not any(reply[9:12]) and size <= 192 and
                 len(reply) == HEADER.size + size + 4 and
                 zlib.crc32(reply[:-4]) == struct.unpack_from('<I', reply, len(reply) - 4)[0])
    except (ValueError, IndexError, struct.error) as error:
        raise RuntimeError('Inspection returned a malformed management reply') from error
    if not valid: raise RuntimeError('Inspection returned a corrupt or mismatched management reply')
    return status, sender, reply[HEADER.size:-4]


def inspect_cache(channel, service=None):
    def request(op, boot, data=b''):
        return management_request(channel, 0, op, boot, data, service)

    result, boot, identity = request(1, 0)
    if result or len(identity) != 120 or struct.unpack_from('<I', identity, 116)[0] != 1:
        raise RuntimeError('Cache inspection could not read the S3 identity')
    result, _, _ = request(13, boot)  # CACHE_INFO: never writes or activates an image.
    if result not in (0, 7, 9): raise RuntimeError(f'Cache inspection failed with management status {result}')
    progress, _, _ = request(19, boot, bytes(4))  # CACHE_PROGRESS with an intentionally absent token.
    if progress not in (7, 9): raise RuntimeError(f'Unexpected cache progress result for an absent token: {progress}')
    try:
        guard = channel.request('transfer-status', 'KMSTRANSFER ', service=service)[-1]
    except TimeoutError as error:
        raise RuntimeError('Update S3 test firmware: transfer-status is required for the Background guard check') from error
    if guard != 'KMSTRANSFER busy=0':
        raise RuntimeError('Read-only cache inspection left the automatic handover update guard blocked: ' + guard)
    return dict(cache_status=result, progress_status=progress, update_guard_clear=True)


def inspect_radio(channel, slot, service=None):
    result, boot, identity = management_request(channel, slot, 1, service=service)
    if result or len(identity) != 120 or struct.unpack_from('<I', identity, 116)[0] != 1:
        raise RuntimeError(f'Receiver {slot}: radio inspection could not read identity')
    if not struct.unpack_from('<I', identity, 112)[0] & 32:
        return dict(slot=slot, boot=f'{boot:08x}', supported=False)
    result, _, data = management_request(channel, slot, 21, boot, service=service)
    if result or len(data) != 80:
        raise RuntimeError(f'Receiver {slot}: radio diagnostic status unavailable')
    values = struct.unpack('<20I', data)
    if values[0] != 1 or values[1] != boot:
        raise RuntimeError(f'Receiver {slot}: radio diagnostic schema/boot mismatch')
    names = ('schema', 'boot', 'sample_ms', 'tx_submitted', 'tx_completed', 'tx_failed', 'tx_slow', 'tx_rejected',
             'max_tx_ms', 'last_tx_ms', 'tx_pending_ms', 'rx_packets', 'last_rx_age_ms', 'max_rx_gap_ms',
             'max_dispatch_ms', 'last_dispatch_ms', 'max_tick_ms', 'rx_queue', 'usb_ready', 'min_heap')
    sample = dict(zip(names, values), slot=slot, supported=True); sample['boot'] = f'{boot:08x}'
    policy = channel.request('receiver-recovery', 'KMSRECOVERY enabled=', service=service)[-1]
    fields = dict(word.split('=', 1) for word in policy.split()[1:] if '=' in word)
    if fields.get('enabled') not in ('0', '1') or fields.get('test_deferred') not in ('0', '1'):
        raise RuntimeError('S3 did not report automatic recovery test deferral')
    sample['recovery_enabled'] = fields['enabled'] == '1'
    sample['recovery_test_deferred'] = fields['test_deferred'] == '1'
    if sample['recovery_enabled'] and not sample['recovery_test_deferred']:
        raise RuntimeError('Automatic receiver recovery must be deferred during stress testing; enable test mode')
    return sample
