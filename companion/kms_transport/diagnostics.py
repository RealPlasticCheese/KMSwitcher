"""Shared device identity and diagnostic wire decoders."""
import struct

IDENTITY = struct.Struct("<48s24s32sIIII")
METRICS = ('radio_rx', 'radio_tx', 'duplicates', 'rejected', 'accepted', 'usb_completed',
           'peak_rx_queue', 'peak_usb_queue', 'max_usb_ms', 'min_heap', 'usb_flags')
EVENTS = ('boot', 'usb_mount', 'usb_suspend', 'usb_resume', 'usb_unmount', 'usb_stall', 'usb_overflow',
          'radio_overflow', 'lease_expired', 'output_clear', 'radio_error', 'update_begin',
          'update_verified', 'update_abort', 'update_confirmed', 'storage_error')

def decode_identity(data):
    if len(data) != IDENTITY.size:
        raise ValueError('Invalid firmware identity size')
    git, utc, digest, size, chip, capabilities, version = IDENTITY.unpack(data)
    if version != 1 or b'\0' not in git or b'\0' not in utc:
        raise ValueError('Invalid firmware identity')
    return dict(git=git.split(b'\0', 1)[0].decode('ascii'), utc=utc.split(b'\0', 1)[0].decode('ascii'),
                sha256=digest.hex(), size=size, chip=chip, capabilities=capabilities,
                confirmed=bool(capabilities & 4))


def decode_summary(data):
    if len(data) != 168:
        raise ValueError('Invalid diagnostic summary size')
    values = struct.unpack('<42I', data)
    signed = lambda value: value if value < 0x80000000 else value - 0x100000000
    return dict(boot=f'{values[0]:08x}', reset_reason=values[1], uptime_ms=values[2] | values[3] << 32,
                metrics=dict(zip(METRICS, values[4:15])), totals=dict(zip(EVENTS, values[15:31])),
                rssi=signed(values[31]), mean_rssi=signed(values[32]), last_radio_ms=values[33],
                events=dict(retained=values[34], capacity=values[35], overwritten=values[36]),
                checkpoints=dict(retained=values[37], capacity=values[38], overwritten=values[39]),
                oldest=dict(boot=f'{values[40]:08x}', uptime_ms=values[41]))


def decode_radio(data):
    if len(data) != 80 or struct.unpack_from('<I', data)[0] != 1:
        raise ValueError('Invalid radio diagnostic status')
    values = struct.unpack('<20I', data)
    names = ('schema', 'boot', 'sample_ms', 'tx_submitted', 'tx_completed', 'tx_failed', 'tx_slow',
             'tx_rejected', 'max_tx_ms', 'last_tx_ms', 'tx_pending_ms', 'rx_packets', 'last_rx_age_ms',
             'max_rx_gap_ms', 'max_dispatch_ms', 'last_dispatch_ms', 'max_tick_ms', 'rx_queue', 'usb_ready', 'min_heap')
    result = dict(zip(names, values)); result['boot'] = f"{result['boot']:08x}"
    return result
