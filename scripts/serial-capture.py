"""Bounded serial capture; leaves BOOT/RESET alone unless explicitly requested."""
import argparse
from pathlib import Path
import time
import queue
import sys
import threading
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--baud', type=int, default=115200)
parser.add_argument('--seconds', type=float, default=10)
parser.add_argument('--output', type=Path)
parser.add_argument('--dtr', action='store_true', help='Assert DTR for a running TinyUSB CDC console')
parser.add_argument('--command', help='Send one controller console command followed by a newline')
parser.add_argument('--status-interval', type=float, help='Poll controller status every N seconds during capture')
parser.add_argument('--interactive', action='store_true', help='Send stdin lines as console commands; .quit ends capture')
parser.add_argument('--uart-reset', action='store_true', help='Pulse EN through an external UART bridge before capture (not native USB)')
parser.add_argument('--reset-hold-ms', type=float, default=100, help='UART reset pulse duration in milliseconds (default: 100)')
args = parser.parse_args()
if args.status_interval is not None and args.status_interval <= 0:
    parser.error('--status-interval must be positive')
if args.reset_hold_ms <= 0:
    parser.error('--reset-hold-ms must be positive')
if args.uart_reset and args.dtr:
    parser.error('--uart-reset requires BOOT/DTR deasserted; do not combine it with --dtr')
connection = serial.Serial()
connection.port = args.port
connection.baudrate = args.baud
connection.timeout = 0.1
connection.dtr = args.dtr
connection.rts = False
connection.open()
if args.command:
    connection.write((args.command + '\n').encode('ascii'))

commands = queue.SimpleQueue()
if args.interactive:
    def read_commands():
        for line in sys.stdin:
            commands.put(line.rstrip('\r\n'))
    threading.Thread(target=read_commands, daemon=True).start()
data = bytearray()
until = time.monotonic() + args.seconds
next_status = time.monotonic()
try:
    if args.uart_reset:
        # Use the same EN pulse as the pinned esptool, with BOOT deasserted.
        from esp_pylib.serial_reset import hard_reset
        hard_reset(connection, hold_delay=args.reset_hold_ms / 1000)
    while time.monotonic() < until:
        if args.status_interval is not None and time.monotonic() >= next_status:
            connection.write(b'status\n')
            next_status = time.monotonic() + args.status_interval
        finish = False
        while not commands.empty():
            command = commands.get_nowait()
            if command == '.quit':
                finish = True
                break
            connection.write((command + '\n').encode('ascii'))
            print(f'> {command}', flush=True)
        if finish:
            break
        chunk = connection.read(max(1, connection.in_waiting))
        data.extend(chunk)
        print(chunk.decode('utf-8', errors='replace'), end='', flush=True)
finally:
    connection.close()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
