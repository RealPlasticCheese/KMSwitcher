"""Bounded local journal reads and quiet recovery notification policy."""
import struct
import secrets
from .protocol import Op, read_words, words

EVENTS = ('Boot', 'USB mounted', 'USB suspended', 'USB resumed', 'USB unmounted', 'USB stalled',
          'USB queue overflow', 'Radio queue overflow', 'Input lease expired', 'Output cleared',
          'Radio fault', 'Update started', 'Update verified', 'Update aborted', 'Update confirmed', 'Storage error')
RADIO_REASONS = ('', 'MAC delivery failed', 'Slow radio transmission', 'Receive gap', 'Delayed dispatch',
                 'Main loop gap', 'Manual restart', 'Automatic restart')


def decode_event(data, boot, token, index, count):
    values = read_words(data, 12)
    if values[:4] != (1, boot, token, index) or values[11] != count or not values[4] or not values[9] or values[10] >= len(EVENTS):
        raise ValueError('Journal changed or returned an invalid record; refresh to retry')
    _, _, _, _, sequence, event_boot, first, last, detail, repeats, event = values[:11]
    description = EVENTS[event]
    if event == 10 and detail >> 28 == 0xa:
        reason = (detail >> 24) & 15
        description = RADIO_REASONS[reason] if reason < len(RADIO_REASONS) else description
        description += f'; {detail & 65535} ms, op {(detail >> 16) & 127}'
        if detail & 0x800000: description += '; diagnostic flash busy'
    return dict(sequence=sequence, boot=f'{event_boot:08x}', first_ms=first, last_ms=last,
                detail=detail, repeats=repeats, event=event, description=description)


class JournalReader:
    """One short USB request per tick; routing and clipboard keep their HID owner."""
    def __init__(self):
        self.state = dict(loading=False, entries=[], error='', supported=False)
        self.phase = None

    def start(self, receiver):
        self.state = dict(loading=True, entries=[], error='', supported=True)
        self.phase, self.receiver = 'begin', receiver

    def cancel(self, message):
        self.phase = None
        self.state.update(loading=False, error=message)

    def tick(self, receiver):
        if not self.phase: return
        if receiver is not self.receiver:
            self.cancel('Dongle reconnected; refresh to read its journal again'); return
        try:
            if self.phase == 'begin':
                info = read_words(receiver.request(Op.LOG_BEGIN, timeout=.2).data, 12)
                schema, self.boot, self.token, self.count, capacity, overwritten, checkpoints, lost, lo, hi, reset, reserved = info
                if schema != 1 or not self.boot or not self.token or not 0 <= self.count <= capacity <= 256 or reserved:
                    raise ValueError('Invalid local journal metadata')
                self.state.update(boot=f'{self.boot:08x}', count=self.count, capacity=capacity, overwritten=overwritten,
                                  checkpoints=checkpoints, checkpoints_overwritten=lost, uptime_ms=lo | hi << 32, reset_reason=reset)
                self.phase = 'read' if self.count else 'end'
            elif self.phase == 'read':
                index = len(self.state['entries'])
                data = receiver.request(Op.LOG_READ, words(self.token, index, self.boot), timeout=.2).data
                self.state['entries'].append(decode_event(data, self.boot, self.token, index, self.count))
                if index + 1 == self.count: self.phase = 'end'
            else:
                receiver.request(Op.LOG_END, words(self.token, 0, self.boot), timeout=.2)
                self.phase = None; self.state['loading'] = False
        except (OSError, RuntimeError, ValueError, struct.error) as error:
            self.cancel(str(error))  # A partial capture is visibly incomplete and cannot be exported.


class RecoveryNotices:
    def __init__(self):
        self.seen, self.last_info, self.last_warning = {}, {}, {}
        self.events = []
        self.serial = 0
        self.session = secrets.randbits(32) or 1
        self.master = None

    def observe(self, data, now, enabled=True):
        schema, master, sequence, kind, slot, at, age, boot, reasons, attempts, selected, reserved = read_words(data, 12)
        if schema != 1 or not master or kind not in (0, 1, 2) or not 1 <= slot <= 7 or reserved:
            raise ValueError('Invalid recovery status')
        if master != self.master:
            self.master = master; self.seen.clear()
        key = master, slot
        previous = self.seen.get(key)
        if previous is not None and not 0 < ((sequence - previous) & 0xffffffff) < 0x80000000:
            return
        self.seen[key] = sequence
        if previous is None or not sequence or not kind or age > 60000:
            return
        record = dict(slot=slot, kind=kind, attempts=attempts, boot=f'{boot:08x}', reasons=reasons)
        self.serial += 1; record['id'] = self.serial
        clock = self.last_info if kind == 1 else self.last_warning
        interval = 600 if kind == 1 else 60
        record['notify'] = enabled and now - clock.get(slot, float('-inf')) >= interval
        if record['notify']: clock[slot] = now
        record['message'] = (f'Dongle {slot} restarted after repeated radio errors. Ready again.' if kind == 1 else
                             f'Dongle {slot} has not recovered. A manual reset may be needed.')
        self.events.append(record); self.events = self.events[-32:]
