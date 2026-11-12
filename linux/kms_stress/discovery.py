"""Read fixture identities and retain saved assignments that need review."""
import copy

from .hardware import SerialChannel, discover_serial, peers, read_identity, receiver_nodes
from .model import Device


def discover_config(config, notify, cancel):
    config = copy.deepcopy(config)
    notify("Discovering UART identities and receivers; no routing/input changes.")

    def diagnostic(kind, data):
        notify(f"{data['port']} [{kind}]: {data['line']}")

    found, problems = discover_serial(diagnostic, cancel)
    if cancel.is_set():
        notify("Discovery stopped; saved assignments retained.")
        return config
    slots, nodes = {}, receiver_nodes()
    masters = [device for role, device in found if role == "master"]
    saved_masters = config.master_devices()
    known_masters = {device.identity.upper(): device for device in saved_masters}
    for device in masters:
        existing = known_masters.get(device.identity.upper())
        if existing:
            existing.uart = device.uart
        else:
            device.name = device.name or "Master"
            device.enabled = not saved_masters and len(masters) == 1
            saved_masters.append(device)
            known_masters[device.identity.upper()] = device
    selected_masters = config.select_master()
    selected = next((device for device in masters if len(selected_masters) == 1
                     and device.identity.upper() == config.master.identity.upper()), None)
    if selected:
        channel = None
        try:
            channel = SerialChannel(selected.uart, diagnostic)
            identity = read_identity(channel)
            if identity.role != "master" or identity.identity != selected.identity:
                raise RuntimeError("Master identity changed while opening inventory connection")
            slots = peers(channel)[0]
        except Exception as error:
            problems.append(f"Master identified, but pairing inventory unavailable: {error}")
        finally:
            if channel:
                channel.close()
        config.master.uart = selected.uart
        config.master.identity = selected.identity
    known = {device.identity.upper(): device for device in config.generators}
    for role, device in found:
        if role != "generator":
            continue
        existing = known.get(device.identity.upper())
        if existing:
            existing.uart = device.uart
        else:
            device.name = f"Generator {len(config.generators) + 1}"
            device.enabled = len(config.generators) < 2
            config.generators.append(device)
            known[device.identity.upper()] = device
    known_slots = {device.slot: device for device in config.receivers}
    for slot, identity in sorted(slots.items()):
        if slot in known_slots:
            if known_slots[slot].identity.upper() != identity:
                notify(f"Slot {slot} changed identity; saved assignment retained for review.")
        else:
            config.receivers.append(Device(identity, f"Slave {slot}", slot=slot, enabled=identity in nodes))
    for problem in problems:
        notify(problem)
    summary = f"Found {len(found)} test UARTs and {len(nodes)} receivers. "
    summary += "Master identified. " if selected else "Master not identified; check its UART/COM connection. "
    summary += f"{len(problems)} connection issue(s); see progress / diagnostics." if problems else "Review enabled devices before starting."
    notify(summary)
    return config
