"""CLI commands share the background helper's HID session and master revision checks."""
import argparse
import json
import logging
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from . import __version__, BUILD_INFO
from . import ipc


def ensure_helper(serial=None):
    try:
        ipc.call("status")
        return
    except RuntimeError:
        pass
    if getattr(sys, "frozen", False):
        # Use the console helper instead of spawning a second tray executable.
        executable = Path(sys.executable).with_name("kmswitcher.exe" if os.name == "nt" else "kmswitcher")
        command = [str(executable), "run"]
    else:
        command = [sys.executable, "-m", "kms_companion", "run"]
    if serial:
        command.extend(["--serial", serial])
    options = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NO_WINDOW
    else:
        options["start_new_session"] = True
    subprocess.Popen(command, **options)
    for _ in range(50):
        time.sleep(.1)
        try:
            ipc.call("status")
            return
        except RuntimeError:
            pass
    raise RuntimeError("Helper failed to start; run 'kmswitcher run' in a terminal for diagnostics")


def parser():
    p = argparse.ArgumentParser(description="KMSwitcher companion: configure through any receiver")
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("--json", action="store_true", help="Machine-readable output; errors go to stderr")
    sub = p.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="Run the background helper in this session")
    run.add_argument("--serial", help="Select a receiver if more than one is attached")
    ui = sub.add_parser("ui", help="Open configuration UI and system tray")
    ui.add_argument("--tray", action="store_true", help="Start with only the tray icon where supported")
    sub.add_parser("status")
    sub.add_parser("stop", help="Stop the background helper and withdraw desktop readiness")
    sub.add_parser("quit", help="Close the tray/settings app and background helper")
    sub.add_parser("build-info", help="Show application version and source identity")
    sub.add_parser("devices", help="List desktop-capable USB receivers")
    sub.add_parser("desktop", help="Read local desktop geometry without changing pointer position")
    sub.add_parser("peers", help="Show station availability and current screens")
    auto = sub.add_parser("auto", help="Enable/disable automatic switching on this computer")
    auto.add_argument("state", choices=("enable", "disable"))
    parking = sub.add_parser("pointer-parking", help="Choose where this computer parks its pointer after switching away")
    from .geometry import POINTER_PARKING_MODES
    parking.add_argument("mode", choices=POINTER_PARKING_MODES)
    select = sub.add_parser("select", help="Manually select receiver slot")
    select.add_argument("slot", type=int, choices=range(1, 8))
    config = sub.add_parser("config")
    cfg = config.add_subparsers(dest="action", required=True)
    cfg.add_parser("show")
    export = cfg.add_parser("export"); export.add_argument("file", type=Path)
    imp = cfg.add_parser("import"); imp.add_argument("file", type=Path)
    imp.add_argument("--expected-revision", type=int, help="Override the revision in the exported document")
    global_auto = cfg.add_parser("auto"); global_auto.add_argument("state", choices=("enable", "disable"))
    station = cfg.add_parser("station"); station.add_argument("slot", type=int, choices=range(1, 8))
    station.add_argument("--x", type=int, required=True); station.add_argument("--y", type=int, required=True)
    station.add_argument("--remove", action="store_true")
    station.add_argument("--width", type=int, help="Fixed station display-area width")
    station.add_argument("--height", type=int, help="Fixed station display-area height")
    prefs = sub.add_parser("preferences", help="Read/write existing S3 everyday preference records")
    prefs.add_argument("--set", action="append", default=[], metavar="NAME=JSON", help="Set a named preference; repeat for multiple changes")
    prefs.add_argument("--expected-revision", type=int)
    startup = sub.add_parser("startup", help="Install/remove per-user login startup")
    startup.add_argument("state", choices=("enable", "disable"))
    return p


def startup_enabled():
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Run") as key:
                winreg.QueryValueEx(key, "KMSwitcher")
            return True
        except OSError:
            return False
    return (Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "autostart/kmswitcher.desktop").exists()


def startup(enable):
    command = [sys.executable] if getattr(sys, "frozen", False) else [sys.executable, "-m", "kms_companion"]
    if getattr(sys, "frozen", False):
        command[0] = str(Path(sys.executable).with_name("kmswitcher-gui.exe" if os.name == "nt" else "kmswitcher-gui"))
    if os.name == "nt" and not getattr(sys, "frozen", False):
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        if pythonw.exists():
            command[0] = str(pythonw)
    command += ["ui", "--tray"]
    if os.name == "nt":
        import winreg
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Run") as key:
            if enable:
                winreg.SetValueEx(key, "KMSwitcher", 0, winreg.REG_SZ, subprocess.list2cmdline(command))
            else:
                try:
                    winreg.DeleteValue(key, "KMSwitcher")
                except FileNotFoundError:
                    pass
    else:
        path = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "autostart/kmswitcher.desktop"
        if enable:
            path.parent.mkdir(parents=True, exist_ok=True)
            # Desktop Exec uses double-quoted arguments, not shell quoting.
            escaped = ' '.join('"' + arg.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"' for arg in command)
            path.write_text("[Desktop Entry]\nType=Application\nName=KMSwitcher\nTryExec=" + command[0] + "\nExec=" + escaped + "\nTerminal=false\n", encoding="utf-8")
        else:
            path.unlink(missing_ok=True)
    return {"startup": enable}


def execute(args):
    if args.command == "build-info":
        return dict(BUILD_INFO, version=__version__)
    if args.command == "quit":
        from PySide6.QtCore import QCoreApplication
        from .ui_instance import send_command
        app = QCoreApplication.instance() or QCoreApplication([])
        if send_command(b"quit"):
            deadline = time.monotonic() + 10
            while send_command(b"ping"):
                if time.monotonic() >= deadline:
                    # Keep its helper alive if the user declined to quit.
                    raise RuntimeError("KMSwitcher is still open; close it before continuing")
                time.sleep(.1)
        try:
            ipc.call("stop")
        except RuntimeError:
            pass
        return {"stopped": True}
    if args.command == "run":
        from .agent import Agent
        server = ipc.Server(Agent(args.serial))
        server.start()
        def stop(*_):
            server.agent.stop_event.set()
        signal.signal(signal.SIGINT, stop)
        signal.signal(signal.SIGTERM, stop)
        try:
            while not server.agent.stop_event.wait(.5):
                pass
        finally:
            server.close()
        return None
    if args.command == "ui":
        from .ui import launch
        return launch(tray_only=args.tray)
    if args.command == "devices":
        from .transport import devices
        return [{k: v for k, v in d.items() if k != "path"} for d in devices()]
    if args.command == "desktop":
        from .desktop import create_desktop
        desktop = create_desktop()
        return {"backend": desktop.name, "ready": desktop.ready(), "screens": [r.as_dict() for r in desktop.screens()], "pointer": desktop.pointer()}
    if args.command == "startup":
        return startup(args.state == "enable")
    if args.command == "status":
        return ipc.call("status")
    if args.command == "stop":
        return ipc.call("stop")
    if args.command == "auto":
        return ipc.call("auto", enabled=args.state == "enable")
    if args.command == "pointer-parking":
        return ipc.call("pointer-parking", mode=args.mode)
    if args.command == "select":
        return ipc.call("select", slot=args.slot)
    if args.command == "peers":
        return ipc.call("peers")
    if args.command == "preferences":
        doc = ipc.call("preferences-get")
        if args.set:
            for setting in args.set:
                key, separator, value = setting.partition("=")
                if not separator or key not in doc["values"]:
                    raise ValueError("Expected a known preference NAME=JSON")
                doc["values"][key] = json.loads(value)
            if args.expected_revision is not None:
                doc["revision"] = args.expected_revision
            return ipc.call("preferences-set", **doc)
        return doc
    if args.command == "config":
        if args.action == "import":
            doc = json.loads(args.file.read_text(encoding="utf-8"))
            from .protocol import encode_config
            encode_config(doc["config"])
            revision = args.expected_revision if args.expected_revision is not None else doc["revision"]
            return ipc.call("config-set", config=doc["config"], revision=revision)
        doc = ipc.call("config-get")
        if args.action == "export":
            args.file.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
            return {"file": str(args.file.resolve()), "revision": doc["revision"]}
        if args.action == "auto":
            doc["config"]["enabled"] = args.state == "enable"
        elif args.action == "station":
            station = doc["config"]["stations"][args.slot - 1]
            station.update(x=args.x, y=args.y, configured=not args.remove)
            if args.width is not None:
                station["width"] = args.width
            if args.height is not None:
                station["height"] = args.height
            if not args.remove and (not station["width"] or not station["height"]):
                peer = ipc.call("peers")[args.slot - 1]
                if not peer["screens"]:
                    raise ValueError("Station has no reported screens; specify --width and --height")
                station["width"] = station["width"] or min(32767, max(r["x"] + r["width"] for r in peer["screens"]))
                station["height"] = station["height"] or min(32767, max(r["y"] + r["height"] for r in peer["screens"]))
        else:
            return doc
        return ipc.call("config-set", **doc)
    raise ValueError("Unknown command")


def main(argv=None):
    args = parser().parse_args(argv)
    if args.command == 'run':
        from .runtime_log import configure
        try:
            configure('helper')
        except OSError:
            pass
    try:
        result = execute(args)
        if args.command == 'ui':
            return result or 0
        if result is not None:
            print(json.dumps(result, indent=None if args.json else 2))
        return 0
    except (RuntimeError, OSError, ValueError, TypeError, KeyError, ImportError) as error:
        if args.command == 'run':
            logging.getLogger(__name__).exception('Background helper failed')
        print(json.dumps({"error": str(error)}) if args.json else f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
