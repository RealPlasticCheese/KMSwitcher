"""Smoke-test a frozen installation without opening a serial or HID device."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def check(app):
    identity = json.loads((app / "release.json").read_text())
    if not (app / "LICENSE").is_file() or not (app / "THIRD-PARTY-NOTICES.md").is_file():
        raise ValueError("Packaged license and third-party notices are required")
    notice_path = app / "third-party-notices"
    inventory = json.loads((notice_path / "inventory.json").read_text(encoding="utf-8"))
    names = {name.lower().replace("_", "-"): name for name in inventory}
    if not {"pyside6-essentials", "shiboken6"} <= names.keys():
        raise ValueError("Packaged Qt notices are incomplete")
    notices = ("Python-LICENSE.txt", "Qt/LGPL-3.0-only.txt", "Qt/GPL-3.0-only.txt")
    if any(not (notice_path / name).is_file() for name in notices):
        raise ValueError("Packaged Python or Qt license texts are missing")
    product = identity.get("product", "companion")
    if (app / "firmware").exists() or (app / "KMSwitcher.html").exists():
        raise ValueError("Applications must not embed firmware or the web console")
    if product != "companion":
        check_maintenance(app, identity, product)
        return
    if (app / ("kms-firmware.exe" if os.name == "nt" else "kms-firmware")).exists():
        raise ValueError("Companion must not include the flashing application")
    suffix = ".exe" if os.name == "nt" else ""
    executable = app / ("kmswitcher" + suffix)
    identity = json.loads((app / "release.json").read_text())
    output = subprocess.check_output([str(executable), "--version"], text=True, timeout=30).strip()
    if output != identity["version"]:
        raise ValueError(f"Packaged version differs: {output}")
    subprocess.run([str(executable), "--json", "build-info"], check=True, timeout=30)
    # Load the platform HID backend in the frozen app. Enumeration does not
    # claim an interface or send reports, and an empty list is valid in CI.
    devices = json.loads(subprocess.check_output([str(executable), "--json", "devices"], text=True, timeout=30))
    if not isinstance(devices, list):
        raise ValueError("Packaged HID discovery did not return a device list")
    # Smoke flags exercise setup and repeated companion UI updates without
    # connecting to a board or starting a helper.
    with tempfile.TemporaryDirectory(prefix="kms-package-smoke-") as temporary:
        environment = dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), KMS_MAINTENANCE_SMOKE="1",
                           LOCALAPPDATA=temporary, XDG_CONFIG_HOME=temporary)
        environment["KMS_COMPANION_SMOKE"] = "1"
        subprocess.run([str(app / ("kmswitcher-gui" + suffix)), "ui"], env=environment, check=True, timeout=30)
        # Exercise the real frozen launcher/worker handoff without starting a
        # helper or opening HID. A clean version-command exit must stay closed.
        environment['KMS_COMPANION_SMOKE'] = '0'
        subprocess.run([str(app / ('kmswitcher-gui' + suffix)), '--version'],
                       env=environment, check=True, timeout=15)
        logs = Path(temporary) / 'KMSwitcher/logs'
        launcher = (logs / 'tray-launcher.log').read_text(encoding='utf-8')
        if 'Starting tray-launcher' not in launcher or 'Restarting tray' in launcher:
            raise ValueError('Packaged tray launcher did not complete a clean worker exit')
        if not (logs / 'tray.log').exists():
            raise ValueError('Packaged tray worker did not initialize fault logging')
    if os.environ.get("XDG_SESSION_TYPE") == "x11" and os.environ.get("DISPLAY"):
        desktop = json.loads(subprocess.check_output([str(executable), "--json", "desktop"], text=True, timeout=30))
        if not desktop["screens"]:
            raise ValueError("Packaged X11 backend did not discover a monitor")
    print("PASS: packaged CLI, release identity, HID discovery and companion UI without embedded firmware")


def check_maintenance(app, identity, product):
    suffix = ".exe" if os.name == "nt" else ""
    executable = app / ("kms-firmware" + suffix)
    target = "stress" if product == "stress" else "regular"
    prefix = [str(executable), "--product", target]
    subprocess.run(prefix + ["ports"], check=True, timeout=30)
    refused = subprocess.run(prefix + ["check"], capture_output=True, text=True, timeout=30)
    if refused.returncode != 1 or "No module" in refused.stderr:
        raise ValueError("Missing external firmware was not reported correctly")
    for chip in ("esp32s2", "esp32s3"):
        stub = json.loads((app / f"_internal/esptool/targets/stub_flasher/1/{chip}.json").read_text())
        if not stub.get("text") or not stub.get("entry"):
            raise ValueError("Packaged flashing stub is missing: " + chip)
    if product != "companion" and (app / ("kmswitcher" + suffix)).exists():
        raise ValueError("Standalone toolkit unexpectedly contains the companion launcher")
    with tempfile.TemporaryDirectory(prefix="kms-maintenance-smoke-") as temporary:
        environment = dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"),
            KMS_MAINTENANCE_SMOKE="1", LOCALAPPDATA=temporary, XDG_CONFIG_HOME=temporary)
        subprocess.run([str(app / ("kms-setup" + suffix)), "--product", target], env=environment, check=True, timeout=30)
        if product == "stress":
            subprocess.run([str(app / "kms-stress"), "--demo", "--profile", str(Path(temporary) / "unused-profile.json"), "--output", str(Path(temporary) / "reports")],
                env=environment, check=True, timeout=30)
    print("PASS: independent " + product + " launchers, external firmware selection and GUI")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    check(parser.parse_args().app.resolve())
