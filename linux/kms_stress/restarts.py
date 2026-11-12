"""Explicit fixture restarts, outside capture and workload execution."""
from contextlib import ExitStack
import os
import re
import struct
import threading
import time
import zlib

from .hardware import SerialChannel, read_identity, receiver_nodes


class RestartCancelled(RuntimeError):
    pass


def device_name(role, device):
    return device.name or (f"Receiver {device.slot}" if role == "receiver" else role.title())


def restart_targets(config, target=None, enabled_only=False):
    devices = [("generator", d) for d in config.generators]
    if not enabled_only or config.preset != "baseline":
        devices += [("receiver", d) for d in config.receivers]
        devices += [("master", d) for d in config.master_devices()]
    if enabled_only:
        devices = [(role, d) for role, d in devices if d.enabled]
    if target is not None:
        if not isinstance(target, dict) or set(target) != {"role", "identity"}:
            raise ValueError("Select a device role and chip identity to restart")
        devices = [(role, d) for role, d in devices
                   if role == target["role"] and d.identity == target["identity"]]
        if len(devices) != 1:
            raise ValueError("The device to restart is missing or ambiguous; discover devices again")
    if not devices:
        raise ValueError("No devices selected to restart")
    if any(not re.fullmatch(r"[0-9A-Fa-f]{12}", d.identity) or
           (role != "receiver" and not d.uart) for role, d in devices):
        raise ValueError("Discover and identify devices before restarting them")
    if len({d.identity.upper() for _, d in devices}) != len(devices):
        raise ValueError("Every device to restart needs a distinct chip identity")
    return devices


def receiver_page(fd):
    import fcntl
    data = bytearray(129)
    data[0] = 126
    fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord("H") << 8) | 0x07, data, True)
    page = data[1:]
    if page[:4] != b"KMT1" or zlib.crc32(page[:124]) != struct.unpack_from("<I", page, 124)[0]:
        raise RuntimeError("Receiver returned an invalid device identity page")
    return struct.unpack_from("<I", page, 4)[0], struct.unpack_from("<I", page, 20)[0]


def receiver_control(device):
    entry = receiver_nodes().get(device.identity.upper())
    if not entry or set(entry.hid) != {0, 1, 2} or len(entry.events) < 2:
        raise RuntimeError(f"{device_name('receiver', device)} is unavailable. Try unplugging and reconnecting it.")
    return os.open(entry.hid[2], os.O_RDWR | os.O_NONBLOCK)


def check_receiver_restart(fd, device):
    before, flags = receiver_page(fd)
    if not flags & 16:
        raise RuntimeError(f"{device_name('receiver', device)} needs updated receiver firmware to use Restart. "
                           "For now, unplug and reconnect it.")
    return before


def restart_receiver(device, cancel, notify, timeout=15):
    import fcntl
    name = device_name("receiver", device)
    fd = receiver_control(device)
    try:
        before = check_receiver_restart(fd, device)
        data = bytearray(129)
        data[0] = 126
        struct.pack_into("<4sII", data, 1, b"KMT1", 7, 0x52535431)
        fcntl.ioctl(fd, (3 << 30) | (len(data) << 16) | (ord("H") << 8) | 0x06, data)
    finally:
        os.close(fd)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cancel.wait(0.2):
            raise RestartCancelled("Device restart cancelled; wait for devices to return before retrying")
        try:
            fd = receiver_control(device)
            try: after, _ = receiver_page(fd)
            finally: os.close(fd)
            if after != before:
                notify(f"{name} restarted and reconnected")
                return dict(role="receiver", identity=device.identity, before=f"{before:08x}", after=f"{after:08x}")
        except (OSError, RuntimeError):
            continue
    raise RuntimeError(f"{name} did not return after restarting. Try unplugging and reconnecting {name}.")


def restart_serial(role, device, channel, cancel, notify, timeout=15):
    name = device_name(role, device)
    before = read_identity(channel)
    # ESP auto-reset: BOOT stays released while EN is pulsed low.
    channel.port.dtr = False
    try:
        channel.port.rts = True
        time.sleep(0.1)
    finally:
        channel.port.rts = False
    if cancel.wait(0.5):
        raise RestartCancelled("Device restart cancelled; wait for devices to return before retrying")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            after = read_identity(channel, attempts=1, timeout=0.5)
            if after.role != role or after.identity != device.identity.upper():
                raise ValueError(f"{name}: identity changed after restart; discover devices again")
            if role == "master" and after.boot == before.boot:
                raise TimeoutError("Waiting for a new master boot")
            notify(f"{name} restarted and is responding")
            return dict(role=role, identity=device.identity, before=before.boot, after=after.boot)
        except (TimeoutError, RuntimeError):
            if cancel.wait(0.2): raise RestartCancelled("Device restart cancelled")
    raise RuntimeError(f"{name} did not return after restarting. Try unplugging and reconnecting {name}.")


def restart_devices(config, notify=lambda _: None, cancel=None, log=lambda *args: None,
                    target=None, enabled_only=False):
    cancel = cancel or threading.Event()
    targets = restart_targets(config, target, enabled_only)
    channels = {}
    results = []
    with ExitStack() as stack:
        # Identify every serial target before any restart; stop sources first.
        serial_devices = [(role, d) for role, d in targets if role != "receiver"]
        for d in config.generators:
            if d.enabled and not any(d.identity == item.identity for _, item in serial_devices):
                serial_devices.append(("generator", d))
        selected = [d for d in config.master_devices() if d.enabled]
        if len(selected) == 1 and config.preset != "baseline" and not any(
                selected[0].identity == d.identity for _, d in serial_devices):
            serial_devices.append(("master", selected[0]))
        for role, device in serial_devices:
            if cancel.is_set(): raise RestartCancelled("Device restart cancelled")
            channel = SerialChannel(device.uart, log)
            stack.callback(channel.close)
            info = read_identity(channel)
            if info.role != role or info.identity != device.identity.upper():
                raise ValueError(f"{device_name(role, device)}: identity mismatch; no devices were restarted")
            channels[device.identity] = channel
        for role, device in targets:
            if role == "receiver":
                fd = receiver_control(device)
                try: check_receiver_restart(fd, device)
                finally: os.close(fd)
        for role, device in serial_devices:
            channel = channels[device.identity]
            if role == "generator": channel.request("bench stop", "KMSGEN STOPPING")
            else: channel.request("test-mode on", "KMSTEST MODE test")
        for role, device in targets:
            if cancel.is_set(): raise RestartCancelled("Device restart cancelled")
            notify(f"Restarting {device_name(role, device)}...")
            if role == "receiver":
                result = restart_receiver(device, cancel, notify)
            else:
                result = restart_serial(role, device, channels[device.identity], cancel, notify)
            results.append(result)
    return results
