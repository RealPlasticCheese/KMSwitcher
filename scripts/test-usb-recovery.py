"""Exercise real S3 USB reconnects and collect timing without resetting UART.

This intentionally interrupts USB input for each cycle. It keeps the selected PC,
does not inject keyboard/mouse input, and writes a JSON report plus a UART log.
Run with the pinned ESP-IDF Python (pyserial is required).
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import time

import serial


def snapshot(connection, emit):
    connection.write(b'status\n')
    result = {'timings': {}}
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = connection.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        emit(line)
        route = re.search(r'link: Selected (\d+),', line)
        if route:
            result['selected'] = int(route[1])
        inputs = re.search(r'USB devices=(\d+), HID interfaces=(\d+),', line)
        if inputs:
            result['devices'], result['interfaces'] = map(int, inputs.groups())
        timing = re.search(r'usb-health: (\w+) completed=(\d+) failed=(\d+) max=(\d+)ms', line)
        if timing:
            name, completed, failed, maximum = timing.groups()
            result['timings'][name.lower()] = dict(completed=int(completed), failed=int(failed), max_ms=int(maximum))
        health = re.search(r'usb-recovery: State=(\w+)', line)
        if health:
            result['state'] = health[1]
        boot = re.search(r'diagnostics: Boot=([0-9a-f]+).*uptime=(\d+)ms', line)
        if boot:
            result['boot'], result['uptime_ms'] = boot[1], int(boot[2])
            if not {'selected', 'interfaces', 'state'} <= result.keys():
                raise RuntimeError('Incomplete status; install USB recovery firmware first')
            return result
    raise TimeoutError('Master did not return status')


def reconnect(connection, emit):
    connection.write(b'usb-recover\n')
    deadline = time.monotonic() + 35
    queued = False
    while time.monotonic() < deadline:
        line = connection.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        emit(line)
        if 'KMSUSB RESULT queued' in line:
            queued = True
        if any(marker in line for marker in ('KMSUSB RESULT busy', 'Automatic recovery stopped', 'Guru Meditation', 'assert failed', 'restart scheduled')):
            raise RuntimeError(f'USB reconnect failed: {line}')
        recovered = re.search(r'usb-recovery: USB recovered in (\d+)ms', line)
        if recovered and queued:
            return int(recovered[1])
    raise TimeoutError('USB inputs were not recovered within 35 seconds')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--cycles', type=int, default=5, choices=range(1, 21), metavar='1..20')
    parser.add_argument('--min-inputs', type=int, default=1, choices=range(1, 9), metavar='1..8')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(started_utc=datetime.now(timezone.utc).isoformat(), port=args.port, cycles=[], passed=False)
    with args.output.with_suffix('.log').open('w', encoding='utf-8') as log:
        def emit(line):
            log.write(line + '\n')
            log.flush()

        connection = serial.Serial()
        connection.port = args.port
        connection.baudrate = 115200
        connection.timeout = 0.2
        connection.write_timeout = 3
        connection.dtr = connection.rts = False
        try:
            connection.open()
            before = snapshot(connection, emit)
            report['before'] = before
            if before['state'] != 'running' or before['interfaces'] < args.min_inputs:
                raise RuntimeError(f'Expected at least {args.min_inputs} active HID interfaces: {before}')
            for cycle in range(args.cycles):
                elapsed = reconnect(connection, emit)
                after = snapshot(connection, emit)
                if (after['boot'] != before['boot'] or after['selected'] != before['selected'] or
                        after['interfaces'] < before['interfaces'] or after['state'] != 'running'):
                    raise RuntimeError(f'Boot, destination or inputs changed during recovery: {after}')
                report['cycles'].append(dict(recovery_ms=elapsed, after=after))
                print(f"Cycle {cycle + 1}: recovered in {elapsed} ms; {after['interfaces']} interfaces, PC {after['selected']}, same S3 boot", flush=True)
            report['passed'] = True
        except Exception as error:
            report['error'] = str(error)
            raise
        finally:
            connection.close()
            report['finished_utc'] = datetime.now(timezone.utc).isoformat()
            args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
