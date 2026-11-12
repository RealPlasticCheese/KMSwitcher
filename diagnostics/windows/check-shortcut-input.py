"""Observe only Ctrl/digit transitions from a chosen Windows Raw Input device.

No keys are injected, suppressed, or remapped. Other key values are not logged.
Use this to verify switch-key suppression and delayed Ctrl replay at the PC.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import os
from pathlib import Path
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device-match', required=True, help='Case-insensitive substring of the Raw Input device path')
parser.add_argument('--seconds', type=float, default=30)
parser.add_argument('--poll-ms', type=float, default=5, help='Requested message-pump interval; timestamps include scheduling jitter')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
if sys.platform != 'win32':
    parser.error('This diagnostic requires Windows')
if args.seconds <= 0 or args.poll_ms <= 0:
    parser.error('--seconds and --poll-ms must be positive')

# Constants below are Windows/HID API values, not device-specific tuning.
WM_INPUT = 0x00FF
RID_INPUT, RIDI_DEVICENAME = 0x10000003, 0x20000007
RIM_TYPEKEYBOARD, RIDEV_INPUTSINK, RIDEV_REMOVE = 1, 0x100, 1
HID_PAGE_GENERIC, HID_USAGE_KEYBOARD = 1, 6
RI_KEY_BREAK, RI_KEY_E0 = 1, 2
VK_CONTROL, VK_LCONTROL, VK_RCONTROL = 0x11, 0xA2, 0xA3
VK_0, VK_9, VK_NUMPAD0, VK_NUMPAD9 = 0x30, 0x39, 0x60, 0x69
PM_REMOVE, HWND_MESSAGE, UINT_ERROR = 1, -3, c.c_uint(-1).value
WNDPROC = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)

class WindowClass(c.Structure):
    _fields_ = [('style', w.UINT), ('proc', WNDPROC), ('class_extra', c.c_int),
                ('window_extra', c.c_int), ('instance', w.HINSTANCE),
                ('icon', w.HANDLE), ('cursor', w.HANDLE), ('background', w.HANDLE),
                ('menu_name', w.LPCWSTR), ('class_name', w.LPCWSTR)]

class RawDevice(c.Structure):
    _fields_ = [('page', w.USHORT), ('usage', w.USHORT), ('flags', w.DWORD), ('window', w.HWND)]

class RawHeader(c.Structure):
    _fields_ = [('type', w.DWORD), ('size', w.DWORD), ('device', w.HANDLE), ('param', w.WPARAM)]

class RawKeyboard(c.Structure):
    _fields_ = [('scan', w.USHORT), ('flags', w.USHORT), ('reserved', w.USHORT),
                ('key', w.USHORT), ('message', w.UINT), ('extra', w.ULONG)]

user = c.WinDLL('user32', use_last_error=True)
kernel = c.WinDLL('kernel32', use_last_error=True)
def api(library, name, parameters, result):
    function = getattr(library, name)
    function.argtypes, function.restype = parameters, result
    return function

get_module = api(kernel, 'GetModuleHandleW', [w.LPCWSTR], w.HMODULE)
register_class = api(user, 'RegisterClassW', [c.POINTER(WindowClass)], w.ATOM)
unregister_class = api(user, 'UnregisterClassW', [w.LPCWSTR, w.HINSTANCE], w.BOOL)
create_window = api(user, 'CreateWindowExW', [w.DWORD, w.LPCWSTR, w.LPCWSTR,
    w.DWORD, c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, c.c_void_p], w.HWND)
destroy_window = api(user, 'DestroyWindow', [w.HWND], w.BOOL)
default_proc = api(user, 'DefWindowProcW', [w.HWND, w.UINT, w.WPARAM, w.LPARAM], c.c_ssize_t)
register_input = api(user, 'RegisterRawInputDevices', [c.POINTER(RawDevice), w.UINT, w.UINT], w.BOOL)
get_input = api(user, 'GetRawInputData', [w.HANDLE, w.UINT, c.c_void_p, c.POINTER(w.UINT), w.UINT], w.UINT)
get_info = api(user, 'GetRawInputDeviceInfoW', [w.HANDLE, w.UINT, c.c_void_p, c.POINTER(w.UINT)], w.UINT)
peek_message = api(user, 'PeekMessageW', [c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT], w.BOOL)
dispatch_message = api(user, 'DispatchMessageW', [c.POINTER(w.MSG)], c.c_ssize_t)

paths, lines, errors = {}, [], []
started = time.monotonic()
def record(line):
    lines.append(line)
    print(line, flush=True)

def read_input(handle):
    size = w.UINT()
    header_size = c.sizeof(RawHeader)
    if get_input(handle, RID_INPUT, None, c.byref(size), header_size) == UINT_ERROR:
        raise c.WinError(c.get_last_error())
    data = c.create_string_buffer(size.value)
    if get_input(handle, RID_INPUT, data, c.byref(size), header_size) == UINT_ERROR:
        raise c.WinError(c.get_last_error())
    if size.value < header_size + c.sizeof(RawKeyboard):
        return
    header = RawHeader.from_buffer(data)
    if header.type != RIM_TYPEKEYBOARD or not header.device:
        return
    if header.device not in paths:
        length = w.UINT()
        if get_info(header.device, RIDI_DEVICENAME, None, c.byref(length)) == UINT_ERROR:
            raise c.WinError(c.get_last_error())
        path = c.create_unicode_buffer(length.value + 1)
        if get_info(header.device, RIDI_DEVICENAME, path, c.byref(length)) == UINT_ERROR:
            raise c.WinError(c.get_last_error())
        paths[header.device] = path.value.casefold()
    if args.device_match.casefold() not in paths[header.device]:
        return
    key = RawKeyboard.from_buffer(data, header_size)
    if key.key in (VK_CONTROL, VK_LCONTROL, VK_RCONTROL):
        label = 'Right Ctrl' if key.flags & RI_KEY_E0 else 'Left Ctrl'
    elif VK_0 <= key.key <= VK_9:
        label = str(key.key - VK_0)
    elif VK_NUMPAD0 <= key.key <= VK_NUMPAD9:
        label = 'Keypad ' + str(key.key - VK_NUMPAD0)
    else:
        return
    edge = 'up' if key.flags & RI_KEY_BREAK else 'down'
    record(f'{(time.monotonic() - started) * 1000:.1f} ms: {label} {edge}')

@WNDPROC
def window_proc(window, message, param, data):
    if message == WM_INPUT:
        try:
            read_input(data)
        except Exception as error:
            errors.append(str(error))
    return default_proc(window, message, param, data)

instance = get_module(None)
name = f'KMSwitcherInputCheck-{os.getpid()}'
window_class = WindowClass(proc=window_proc, instance=instance, class_name=name)
if not register_class(c.byref(window_class)):
    raise c.WinError(c.get_last_error())
window = None
try:
    window = create_window(0, name, None, 0, 0, 0, 0, 0, HWND_MESSAGE, None, instance, None)
    if not window:
        raise c.WinError(c.get_last_error())
    registration = RawDevice(HID_PAGE_GENERIC, HID_USAGE_KEYBOARD, RIDEV_INPUTSINK, window)
    if not register_input(c.byref(registration), 1, c.sizeof(registration)):
        raise c.WinError(c.get_last_error())
    record(f'Watching only Ctrl/digit edges from {args.device_match}; poll={args.poll_ms:g}ms')
    message = w.MSG()
    while time.monotonic() - started < args.seconds:
        while peek_message(c.byref(message), None, 0, 0, PM_REMOVE):
            dispatch_message(c.byref(message))
        if errors:
            raise RuntimeError(errors[0])
        time.sleep(args.poll_ms / 1000)
finally:
    if window:
        registration = RawDevice(HID_PAGE_GENERIC, HID_USAGE_KEYBOARD, RIDEV_REMOVE, None)
        register_input(c.byref(registration), 1, c.sizeof(registration))
        destroy_window(window)
    unregister_class(name, instance)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
