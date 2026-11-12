"""Native Linux packages for independent maintenance and stress applications."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def executable(path, content):
    path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content, encoding="utf-8"); path.chmod(0o755)


def package(app, identity, output, product):
    name = "kmswitcher-" + product
    title = "KMSwitcher Stress Toolkit" if product == "stress" else "KMSwitcher Firmware Setup"
    download_name = name if product == "stress" else "kmswitcher-firmware-setup"
    output.mkdir(parents=True, exist_ok=True)
    stage = ROOT / "build" / ("linux-package-" + product)
    if stage.exists(): shutil.rmtree(stage)
    payload = stage / "portable/payload"
    shutil.copytree(app, payload / "opt" / name)
    launch = "kms-stress-toolkit" if product == "stress" else "kms-setup"
    arguments = " --product stress" if product == "stress" else ""
    executable(payload / "usr/bin" / launch, f'#!/bin/sh\nexec /opt/{name}/kms-setup{arguments} "$@"\n')
    cli = "kms-stress-firmware" if product == "stress" else "kms-firmware"
    executable(payload / "usr/bin" / cli, f'#!/bin/sh\nexec /opt/{name}/kms-firmware{arguments} "$@"\n')
    if product == "stress":
        executable(payload / "usr/bin/kms-stress", '#!/bin/sh\nexec /opt/kmswitcher-stress/kms-stress "$@"\n')
    desktop = payload / f"usr/share/applications/{name}.desktop"; desktop.parent.mkdir(parents=True)
    desktop.write_text(f"[Desktop Entry]\nType=Application\nName={title}\nExec=/opt/{name}/kms-setup{arguments}\nIcon=input-keyboard\nTerminal=false\nCategories=Utility;Settings;\n")
    rule = ("73" if product == "stress" else "72") + "-" + name + ".rules"
    rules = payload / "usr/lib/udev/rules.d" / rule; rules.parent.mkdir(parents=True)
    shutil.copy2(ROOT / ("packaging/linux/70-kmswitcher-stress.rules" if product == "stress" else "companion/70-kmswitcher.rules"), rules)
    reload = (ROOT / "packaging/linux/reload-udev.sh").read_text()
    idle = f'#!/bin/sh\nset -eu\nif command -v fuser >/dev/null 2>&1; then\n  for binary in /opt/{name}/kms-*; do\n    if [ -f "$binary" ] && fuser "$binary" >/dev/null 2>&1; then\n      echo "Close {title} and wait for firmware/test operations to finish first." >&2\n      exit 1\n    fi\n  done\nfi\n'
    remove = f'rm -rf -- /opt/{name}\nrm -f -- /usr/bin/{launch} /usr/bin/{cli} /usr/share/applications/{name}.desktop /usr/lib/udev/rules.d/{rule}\n'
    if product == "stress": remove += 'rm -f -- /usr/bin/kms-stress\n'
    executable(payload / "opt" / name / "uninstall-portable.sh", '#!/bin/sh\nset -eu\n[ "$(id -u)" -eq 0 ] || { echo "Run with sudo" >&2; exit 1; }\n' + idle.replace("#!/bin/sh\n", "") + remove + reload.replace("#!/bin/sh\n", ""))
    cleanup = f"rm -rf -- /opt/{name}/firmware\nrm -f -- /opt/{name}/KMSwitcher.html\n"
    native_guard = f'if command -v dpkg-query >/dev/null 2>&1 && dpkg-query -W -f=\'${{Status}}\' {name} 2>/dev/null | grep -q "install ok installed"; then\n  echo "Use your package manager to update this installed package." >&2; exit 1\nfi\nif command -v rpm >/dev/null 2>&1 && rpm -q {name} >/dev/null 2>&1; then\n  echo "Use your package manager to update this installed package." >&2; exit 1\nfi\n'
    executable(stage / "portable/install.sh", '#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\nif [ "$(id -u)" -ne 0 ]; then exec sudo sh "$0" "$@"; fi\n' + native_guard + idle.replace("#!/bin/sh\n", "") + cleanup + 'cp -a payload/. /\n' + reload.replace("#!/bin/sh\n", "") + f'echo "Installed {title}. Open it from the application menu."\n')
    shutil.copy2(ROOT / "docs/installing.md", stage / "portable/START-HERE.md")
    with tarfile.open(output / f"{download_name}-{identity['version']}-linux-x64.tar.gz", "w:gz") as archive:
        archive.add(stage / "portable", arcname=name)
    (payload / "opt" / name / "uninstall-portable.sh").unlink()
    spec = importlib.util.spec_from_file_location("linux_packaging", ROOT / "scripts/package-linux.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    control = payload / "DEBIAN"; control.mkdir()
    (control / "control").write_text(f"Package: {name}\nVersion: {identity['deb_version']}\nArchitecture: amd64\nMaintainer: Frank Tzanabetis\nHomepage: https://plasticcheese.com/\nSection: utils\nPriority: optional\nDepends: {', '.join(module.DEB_DEPENDENCIES)}\nDescription: {title}\n Import a separate firmware ZIP with the independent updater.\n")
    executable(control / "postinst", reload); executable(control / "postrm", reload)
    executable(control / "preinst", idle + cleanup); executable(control / "prerm", idle)
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(payload), str(output / f"{download_name}_{identity['deb_version']}_amd64.deb")], check=True)
    shutil.rmtree(control)
    top = stage / "rpm"
    for directory in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"): (top / directory).mkdir(parents=True)
    rpm = top / "SPECS" / (name + ".spec")
    rpm.write_text(f"Name: {name}\nVersion: {identity['rpm_version']}\nRelease: {identity['rpm_release']}\nSummary: {title}\nLicense: MIT and bundled third-party licenses\nURL: https://plasticcheese.com/\nPackager: Frank Tzanabetis\nBuildArch: x86_64\nAutoReqProv: no\nRequires: {', '.join(module.RPM_DEPENDENCIES)}\n%description\n{title}, firmware and device setup.\n%install\nmkdir -p %{{buildroot}}\ncp -a {payload}/. %{{buildroot}}/\n%pre\n" + idle.replace("#!/bin/sh\n", "") + cleanup + "\n%preun\n" + idle.replace("#!/bin/sh\n", "") + "\n%post\n" + reload.replace("#!/bin/sh\n", "") + "\n%postun\n" + reload.replace("#!/bin/sh\n", "") + f"\n%files\n/opt/{name}\n/usr/bin/{launch}\n/usr/bin/{cli}\n" + ("/usr/bin/kms-stress\n" if product == "stress" else "") + f"/usr/share/applications/{name}.desktop\n/usr/lib/udev/rules.d/{rule}\n")
    subprocess.run(["rpmbuild", "-bb", "--define", f"_topdir {top}", "--define", "__os_install_post %{nil}", "--define", "_build_id_links none", str(rpm)], check=True)
    for path in (top / "RPMS").rglob("*.rpm"):
        shutil.copy2(path, output / path.name.replace(name + "-", download_name + "-", 1))
    (output / "release.json").write_text(json.dumps(dict(identity, product=product), indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", choices=("setup", "stress"), required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.app.resolve(), json.loads(args.identity.read_text()), args.output.resolve(), args.product)
