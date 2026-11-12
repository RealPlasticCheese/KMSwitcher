"""Read-only, chunked flash backup using the pinned esptool (no installed-tool edits).

The stock 64-packet read window stalled on this S2's native USB connection.
Override only that window in this process, keeping one acknowledged sector in
flight, and verify both each chunk and the complete image against flash MD5.
This approach also stalled on the connected S2; it remains an investigation
tool, not a verified workaround. No output image is saved on an incomplete read.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import types

from esptool.targets.esp32s2 import ESP32S2ROM
from esptool.targets.esp32s3 import ESP32S3ROM

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--chip', choices=('esp32s2', 'esp32s3'), required=True)
parser.add_argument('--size', type=lambda s: int(s, 0), required=True, help='Detected flash capacity in bytes')
parser.add_argument('--length', type=lambda s: int(s, 0), help='Read only this many bytes from offset zero')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--chunk-bytes', type=lambda s: int(s, 0), default=0x10000)
parser.add_argument('--window', type=int, default=1)
args = parser.parse_args()
length = args.length if args.length is not None else args.size
if args.output.exists():
    raise SystemExit('Refusing to overwrite an existing backup.')
if args.size <= 0 or not 0 < length <= args.size or args.chunk_bytes <= 0 or args.chunk_bytes % 4096 or not 1 <= args.window <= 64:
    raise SystemExit('Use positive flash size, sector-aligned chunks, and window 1-64.')
loader = ESP32S2ROM if args.chip == 'esp32s2' else ESP32S3ROM
esp = loader(args.port)
try:
    # The board must already be in ROM download mode. S2 native USB can disappear
    # if classic UART reset lines are toggled during a direct API connection.
    esp.connect('no-reset')
    esp = esp.run_stub()
    original = esp.check_command
    def command(self, description, op=None, data=b'', *pos, **kwargs):
        if op == self.ESP_CMDS['READ_FLASH']:
            offset, length, sector, _ = struct.unpack('<IIII', data)
            data = struct.pack('<IIII', offset, length, sector, args.window)
        return original(description, op, data, *pos, **kwargs)
    esp.check_command = types.MethodType(command, esp)
    esp.flash_set_parameters(args.size)
    image = bytearray()
    for offset in range(0, length, args.chunk_bytes):
        n = min(args.chunk_bytes, length - offset)
        image.extend(esp.read_flash(offset, n))
        print(f'Read {len(image):#x}/{length:#x}', flush=True)
    if hashlib.md5(image).hexdigest().lower() != esp.flash_md5sum(0, length).lower():
        raise SystemExit('Whole-image flash digest mismatch; no backup saved.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    metadata = {'chip': args.chip, 'mac': ':'.join(f'{v:02x}' for v in esp.read_mac()),
                'bytes': len(image), 'flash_capacity': args.size, 'offset': 0,
                'sha256': hashlib.sha256(image).hexdigest(),
                'verified_against_flash_md5': True, 'chunk_bytes': args.chunk_bytes, 'window': args.window}
    args.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata), flush=True)
finally:
    esp._port.close()
