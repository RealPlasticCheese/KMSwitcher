"""Named settings for the existing version-4 S3 preference record."""
import struct
import zlib

TRIGGERS = ("", "Ctrl", "Alt", "Shift", "Scroll Lock", "Insert", "Pause", "Num Lock")
COLOURS = (('Off', '#555555'), ('Red', '#ed2727'), ('Green', '#2cb542'), ('Yellow', '#ffdf37'),
           ('Blue', '#3888ff'), ('Magenta', '#dc4fdd'), ('Cyan', '#38c9db'), ('White', '#ffffff'))
LABELS = {'normal_led': 'Show selected dongle colour', 'brightness': 'Controller LED brightness',
          'keyboard_leds': 'Synchronise keyboard lock lights', 'boot_slot': 'Startup dongle',
          'jiggler_seconds': 'Mouse jiggler interval (seconds)', 'jiggler_distance': 'Mouse jiggler distance (counts)',
          'double_ms': 'Double-tap window (ms)', 'select_ms': 'Selection timeout (ms)'}
LIMITS = {"trigger": (1, 7), "brightness": (1, 255), "boot_slot": (0, 7),
          "jiggler_seconds": (1, 65535), "jiggler_distance": (1, 127),
          "double_ms": (100, 1000), "select_ms": (500, 15000)}


def decode(data):
    if len(data) != 32 or data[:5] != b"KMP1\x04" or zlib.crc32(data[:28]) != struct.unpack_from("<I", data, 28)[0]:
        raise ValueError("Unsupported or corrupt preference record")
    return dict(trigger=data[5], normal_led=bool(data[6]), brightness=data[7], keyboard_leds=bool(data[8]),
                boot_slot=data[9], jiggler_seconds=struct.unpack_from("<H", data, 10)[0],
                double_ms=struct.unpack_from("<I", data, 12)[0], select_ms=struct.unpack_from("<I", data, 16)[0],
                slot_colors=list(data[20:27]), jiggler_distance=data[27])


def encode(values):
    for name, (low, high) in LIMITS.items():
        if type(values.get(name)) is not int or not low <= values[name] <= high:
            raise ValueError(f"{name} must be an integer from {low} to {high}")
    for name in ("normal_led", "keyboard_leds"):
        if type(values.get(name)) is not bool:
            raise ValueError(f"{name} must be boolean")
    colors = values.get("slot_colors", [])
    if len(colors) != 7 or any(type(n) is not int or not 0 <= n <= 7 for n in colors):
        raise ValueError("slot_colors must contain seven colour values from 0 to 7")
    data = bytearray(32)
    data[:5] = b"KMP1\x04"
    data[5:10] = bytes((values["trigger"], values["normal_led"], values["brightness"], values["keyboard_leds"], values["boot_slot"]))
    struct.pack_into("<HII", data, 10, values["jiggler_seconds"], values["double_ms"], values["select_ms"])
    data[20:27] = bytes(colors); data[27] = values["jiggler_distance"]
    struct.pack_into("<I", data, 28, zlib.crc32(data[:28]))
    return bytes(data)
