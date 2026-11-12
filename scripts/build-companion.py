"""Freeze independently versioned applications; firmware is distributed separately."""
import argparse
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from packaging.requirements import Requirement

ROOT = Path(__file__).resolve().parents[1]


def notices(destination, product="companion"):
    destination.mkdir(parents=True, exist_ok=True)
    pending = ["hidapi", "PySide6-Essentials", "pyinstaller", "cryptography"]
    if product != "companion": pending.append("esptool")
    if product == "stress": pending += ["evdev", "pyudev"]
    if product == "companion" and sys.platform.startswith("linux"):
        pending.append("dbus-next")
    inventory = {}
    while pending:
        name = pending.pop()
        dist = metadata.distribution(name)
        key = dist.metadata["Name"]
        if key in inventory:
            continue
        inventory[key] = dict(version=dist.version, license=dist.metadata.get("License-Expression", dist.metadata.get("License", "See license files")))
        for filename in dist.files or []:
            if any(part.lower().startswith(("license", "copying", "copyright", "notice")) for part in filename.parts):
                path = Path(dist.locate_file(filename))
                if path.is_file():
                    target = destination / key / filename
                    # Distribution metadata paths may use ../ to refer to scripts.
                    if target.resolve().is_relative_to(destination.resolve()):
                        target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
        for spec in dist.requires or []:
            requirement = Requirement(spec)
            if requirement.marker is None or requirement.marker.evaluate({"extra": ""}):
                pending.append(requirement.name)
    python_version = f"python{sys.version_info.major}.{sys.version_info.minor}"
    prefix = Path(sys.base_prefix)
    for candidate in (prefix / "LICENSE.txt", prefix / "lib" / python_version / "LICENSE.txt",
                      prefix / "share/doc" / f"lib{python_version}-stdlib" / "copyright"):
        if candidate.is_file():
            shutil.copy2(candidate, destination / "Python-LICENSE.txt")
            break

    qt = metadata.version("PySide6-Essentials")
    qt_minor = ".".join(qt.split(".")[:2])
    qt_source = f"https://download.qt.io/official_releases/qt/{qt_minor}/{qt}/single/qt-everywhere-src-{qt}.tar.xz"
    pyside_source = ("https://download.qt.io/official_releases/QtForPython/pyside6/"
                     f"PySide6-{qt}-src/pyside-setup-everywhere-src-{qt}.tar.xz")
    shutil.copytree(ROOT / "packaging/qt-notices", destination / "Qt", dirs_exist_ok=True)
    (destination / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
    source = (f"Qt {qt} source\n{qt_source}\n\nPySide6 {qt} source\n{pyside_source}\n")
    if product != "companion":
        source += ("\nesptool corresponding source is included with the matching GitHub release.\n"
                   "https://github.com/RealPlasticCheese/KMSwitcher/releases\n")
    (destination / "README.txt").write_text(
        "Third-party package names, versions and licenses are listed in inventory.json.\n"
        "Qt and PySide6 are redistributed under LGPL-3.0. Their shared libraries remain replaceable.\n"
        "See the Qt folder for the LGPL and GPL license texts.\n\n" + source, encoding="utf-8")



def build(identity, output, product="companion"):
    if product == "stress" and sys.platform != "linux":
        raise ValueError("Build the hardware Stress Toolkit on Linux")
    source = ROOT / ("build/package-source-" + product)
    if source.exists():
        # This fixed generated directory contains only this task's staging files.
        if not source.resolve().is_relative_to((ROOT / "build").resolve()):
            raise ValueError("Package staging directory escapes the build workspace")
        shutil.rmtree(source)
    source.mkdir(parents=True)
    if product != "companion":
        shutil.copytree(ROOT / "companion/kms_update", source / "kms_update", ignore=shutil.ignore_patterns("__pycache__"))
    if product == "companion":
        shutil.copytree(ROOT / "companion/kms_companion", source / "kms_companion", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(ROOT / "companion/kms_transport", source / "kms_transport", ignore=shutil.ignore_patterns("__pycache__"))
    package_identity = dict(identity)
    if product == "companion":
        (source / "kms_companion/build-info.json").write_text(json.dumps(package_identity, indent=2))
        for name, module in (("cli", "cli"), ("gui", "gui")):
            (source / f"{name}-entry.py").write_text(f"from kms_companion.{module} import main\nraise SystemExit(main())\n")
    if product == "stress":
        shutil.copytree(ROOT / "linux/kms_stress", source / "kms_stress", ignore=shutil.ignore_patterns("__pycache__"))
        (source / "kms_stress/build-info.json").write_text(json.dumps(package_identity, indent=2))
        (source / "stress-entry.py").write_text("from kms_stress.__main__ import main\nraise SystemExit(main())\n")
    if product != "companion":
        for name, module in (("firmware", "__main__"), ("setup", "gui")):
            (source / f"{name}-entry.py").write_text(f"from kms_update.{module} import main\nraise SystemExit(main())\n")
    environment = dict(os.environ, KMS_PACKAGE_SOURCE=str(source), KMS_PACKAGE_PRODUCT=product)
    if sys.platform == "win32":
        numbers = tuple(int(n) for n in identity["windows_version"].split("."))
        version_file = source / "windows-version.txt"
        version_file.write_text("VSVersionInfo(ffi=FixedFileInfo(filevers=" + repr(numbers) + ", prodvers=" + repr(numbers) +
            ", mask=0x3f, flags=0, OS=0x40004, fileType=1, subtype=0, date=(0,0)), "
            "kids=[StringFileInfo([StringTable('040904B0', [StringStruct('ProductName', 'KMSwitcher'), StringStruct('CompanyName', 'Frank Tzanabetis'), StringStruct('Comments', 'https://plasticcheese.com/'), "
            f"StringStruct('ProductVersion', {identity['version']!r}), StringStruct('FileVersion', {identity['version']!r})" +
            "])]), VarFileInfo([VarStruct('Translation', [1033,1200])])])")
        environment["KMS_WINDOWS_VERSION_FILE"] = str(version_file)
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--distpath", str(output),
                    "--workpath", str(ROOT / ("build/pyinstaller-" + product)), str(ROOT / "packaging/companion.spec")],
                   cwd=ROOT, env=environment, check=True)
    app = output / {"companion": "kmswitcher", "setup": "kmswitcher-setup", "stress": "kmswitcher-stress"}[product]
    shutil.copy2(ROOT / "docs/installing.md", app / "START-HERE.md")
    if product == "companion":
        shutil.copy2(ROOT / "docs/computer-power.md", app / "computer-power.md")
    shutil.copy2(ROOT / "companion/70-kmswitcher.rules", app / "70-kmswitcher.rules")
    notices(app / "third-party-notices", product)
    shutil.copy2(ROOT / "LICENSE", app / "LICENSE")
    shutil.copy2(ROOT / "THIRD-PARTY-NOTICES.md", app / "THIRD-PARTY-NOTICES.md")
    (app / "release.json").write_text(json.dumps(dict(identity, product=product), indent=2) + "\n")
    collection = os.environ.get("SYSTEM_COLLECTIONURI")
    firmware_definition = os.environ.get("KMS_FIRMWARE_PIPELINE_ID")
    if collection and firmware_definition and product != "companion":
        address = collection.rstrip("/") + "/" + os.environ["SYSTEM_TEAMPROJECTID"]
        (app / "release-source.json").write_text(json.dumps(dict(azure=address, definition=int(firmware_definition))))
    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=Path, default=ROOT / "build/release/version.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/app")
    parser.add_argument("--product", choices=("companion", "setup", "stress"), default="companion")
    args = parser.parse_args()
    print(build(json.loads(args.identity.read_text()), args.output, args.product))
