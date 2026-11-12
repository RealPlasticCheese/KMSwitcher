"""Incremental diagnostic reads; each IPC call yields the HID owner to normal work."""
import struct
import time
import zlib

from kms_transport.diagnostics import decode_identity, decode_summary, decode_radio

RESULTS = ('ok', 'bad request', 'busy', 'receiver restarted; refresh', 'unexpected offset', 'integrity failure',
           'storage failure', 'unsupported firmware', 'already current', 'expired session', 'incompatible', 'timeout/unreachable')


class RemoteDiagnostics:
    def __init__(self, call, slot, *, cancelled=lambda: False):
        self.call, self.slot, self.boot = call, slot, 0
        self.cancelled = cancelled

    def request(self, operation, data=b'', offset=0, *, release=False):
        if not release and self.cancelled(): raise RuntimeError('History read cancelled')
        token = self.call('device-remote-start', slot=self.slot, operation=operation,
                          data=data.hex(), offset=offset, boot=self.boot)['token']
        result, total, sender = bytearray(), None, None
        deadline = time.monotonic() + 8
        while total is None or len(result) < total:
            if not release and self.cancelled(): raise RuntimeError('History read cancelled')
            row = self.call('device-remote-read', token=token, offset=len(result))
            if row['pending']:
                if time.monotonic() >= deadline: raise RuntimeError('Remote diagnostics timed out; refresh before retrying')
                time.sleep(.05); continue
            if row['status']: raise RuntimeError(RESULTS[row['status']])
            if total is not None and (total, sender) != (row['length'], row['boot']):
                raise ValueError('Diagnostic response changed during read')
            total, sender = row['length'], row['boot']
            if not sender or (self.boot and sender != self.boot): raise ValueError('Receiver restarted; refresh')
            chunk = bytes.fromhex(row['data'])
            if len(chunk) != min(32, total - len(result)): raise ValueError('Invalid diagnostic chunk')
            result.extend(chunk)
        self.boot = sender
        return bytes(result)

    def status(self):
        identity = decode_identity(self.request(1))
        identity['boot'] = f'{self.boot:08x}'
        summary = decode_summary(self.request(2))
        if summary['boot'] != identity['boot']: raise ValueError('Receiver restarted during inspection')
        result = dict(slot=self.slot, identity=identity, diagnostics=summary)
        if identity['capabilities'] & 32:
            radio = decode_radio(self.request(21))
            if radio['boot'] != identity['boot']: raise ValueError('Radio status belongs to a different boot')
            result['radio'] = radio
        return result

    def history(self, progress=lambda done, total: None):
        self.request(1)
        header = self.request(3)
        if len(header) != 12: raise ValueError('Invalid history snapshot')
        token, size, expected = struct.unpack('<III', header)
        if not token or not 0 < size <= 98304: raise ValueError('History exceeds the supported size')
        data = bytearray()
        try:
            while len(data) < size:
                chunk = self.request(4, struct.pack('<I', token), len(data))
                if not chunk or len(chunk) > size - len(data): raise ValueError('Invalid history chunk')
                data.extend(chunk); progress(len(data), size)
            if zlib.crc32(data) != expected: raise ValueError('History checksum failed; no archive was saved')
            return bytes(data)
        finally:
            try: self.request(4, struct.pack('<I', token), size, release=True)
            except (RuntimeError, ValueError, OSError): pass

    def clear(self):
        self.request(1); self.request(5)
        return dict(cleared=True)


def usb_inventory(call):
    result = call('device-usb-snapshot')
    token = result['token']
    result['devices'] = [call('device-usb-device', token=token, index=i) for i in range(result['devices'])]
    result['interfaces'] = [call('device-usb-interface', token=token, index=i) for i in range(result['interfaces'])]
    addresses = {d['address'] for d in result['devices']}
    if any(i['address'] not in addresses for i in result['interfaces']): raise ValueError('USB inventory changed; refresh')
    return result
