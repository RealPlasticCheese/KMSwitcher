"""Build DEB, RPM and an installable tar archive from the same frozen application."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
DEB_DEPENDENCIES = (
    "libc6 (>= 2.35)", "libstdc++6", "libx11-6", "libx11-xcb1", "libxext6", "libxi6",
    "libxrender1", "libxrandr2", "libxfixes3", "libsm6", "libice6", "libxcb1",
    "libxcb-cursor0", "libxcb-icccm4", "libxcb-keysyms1", "libxcb-image0",
    "libxcb-render0", "libxcb-render-util0", "libxcb-shape0", "libxcb-randr0",
    "libxcb-xfixes0", "libxcb-sync1", "libxcb-shm0", "libxcb-xkb1", "libxcb-glx0",
    "libxkbcommon-x11-0", "libegl1", "libopengl0", "libgl1", "libglib2.0-0",
    "libdbus-1-3", "libfontconfig1", "libfreetype6", "libusb-1.0-0", "libudev1",
    "x11-xserver-utils", "psmisc",
)
RPM_DEPENDENCIES = (
    "glibc >= 2.35", "libstdc++", "libX11", "libX11-xcb", "libXext", "libXi",
    "libXrender", "libXrandr", "libXfixes", "libSM", "libICE", "libxcb",
    "xcb-util-cursor", "xcb-util-wm", "xcb-util-keysyms", "xcb-util-image",
    "xcb-util-renderutil", "libxkbcommon-x11", "mesa-libEGL", "libglvnd-opengl",
    "libGL.so.1()(64bit)", "glib2", "dbus-libs", "fontconfig", "freetype",
    "libusb1", "systemd-libs", "xrandr", "psmisc",
)


def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8"); path.chmod(0o755)


def package(app, identity, output):
    output.mkdir(parents=True, exist_ok=True)
    stage = ROOT / "build/linux-package"
    if stage.exists():
        shutil.rmtree(stage)
    payload = stage / "portable/payload"
    shutil.copytree(app, payload / "opt/kmswitcher")
    for binary in ("kmswitcher", "kmswitcher-gui"):
        target = payload / "usr/bin" / binary
        target.parent.mkdir(parents=True, exist_ok=True); target.symlink_to("/opt/kmswitcher/" + binary)
    for source, target in ((ROOT / "packaging/linux/kmswitcher.desktop", "usr/share/applications/kmswitcher.desktop"),
                           (ROOT / "packaging/linux/kmswitcher.svg", "usr/share/icons/hicolor/scalable/apps/kmswitcher.svg"),
                           # A distinct basename avoids legacy /etc/udev rules
                           # shadowing the package's new programming-port access.
                           (ROOT / "companion/70-kmswitcher.rules", "usr/lib/udev/rules.d/71-kmswitcher-companion.rules")):
        path = payload / target; path.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, path)
    stop = (ROOT / "packaging/linux/stop-installed.sh").read_text()
    reload = (ROOT / "packaging/linux/reload-udev.sh").read_text()
    cleanup = "rm -rf -- /opt/kmswitcher/firmware\nrm -f -- /opt/kmswitcher/KMSwitcher.html /opt/kmswitcher/kms-firmware /opt/kmswitcher/kms-setup\n"
    for name in ("install.sh", "stop-installed.sh", "reload-udev.sh"):
        executable(stage / "portable" / name, (ROOT / "packaging/linux" / name).read_text())
    shutil.copy2(ROOT / "docs/installing.md", stage / "portable/START-HERE.md")
    executable(payload / "opt/kmswitcher/uninstall-portable.sh", "#!/bin/sh\nset -eu\n"
        "[ \"$(id -u)\" -eq 0 ] || { echo 'Run this uninstaller with sudo.' >&2; exit 1; }\n" +
        stop.replace("#!/bin/sh\n", "") +
        "rm -rf -- /opt/kmswitcher\nrm -f -- /usr/bin/kmswitcher /usr/bin/kmswitcher-gui "
        "/usr/share/applications/kmswitcher.desktop /usr/share/icons/hicolor/scalable/apps/kmswitcher.svg "
        "/usr/lib/udev/rules.d/71-kmswitcher-companion.rules\n" + reload.replace("#!/bin/sh\n", ""))
    # Native packages own removal; do not offer a competing portable uninstaller there.
    portable_name = f"KMSwitcher-Companion-{identity['version']}-linux-x64"
    with tarfile.open(output / (portable_name + ".tar.gz"), "w:gz") as archive:
        archive.add(stage / "portable", arcname=portable_name)
    (payload / "opt/kmswitcher/uninstall-portable.sh").unlink()
    control = payload / "DEBIAN"; control.mkdir()
    (control / "control").write_text(f"Package: kmswitcher\nVersion: {identity['deb_version']}\nArchitecture: amd64\n"
        "Maintainer: Frank Tzanabetis\nHomepage: https://plasticcheese.com/\nSection: utils\nPriority: optional\n"
        f"Depends: {', '.join(DEB_DEPENDENCIES)}\n"
        "Description: KMSwitcher Companion\n"
        " Includes GUI, tray and command-line tools for desktop switching.\n")
    executable(control / "preinst", stop + cleanup)
    executable(control / "prerm", stop)
    executable(control / "postinst", reload)
    executable(control / "postrm", reload)
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(payload),
                    str(output / f"kmswitcher-companion_{identity['deb_version']}_amd64.deb")], check=True)
    shutil.rmtree(control)
    top = stage / "rpm"
    for name in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"):
        (top / name).mkdir(parents=True)
    spec = top / "SPECS/kmswitcher.spec"
    spec.write_text(f"Name: kmswitcher\nVersion: {identity['rpm_version']}\nRelease: {identity['rpm_release']}\n"
        "Summary: KMSwitcher Companion\nLicense: Unspecified\nURL: https://plasticcheese.com/\nBuildArch: x86_64\n"
        f"AutoReqProv: no\nRequires: {', '.join(RPM_DEPENDENCIES)}\n"
        "%description\nDesktop layout, tray and command-line tools.\n"
        "%install\nmkdir -p %{buildroot}\ncp -a " + str(payload) + "/. %{buildroot}/\n"
        "%pre\n" + stop.replace("#!/bin/sh\n", "") + cleanup + "\n%preun\n" + stop.replace("#!/bin/sh\n", "") +
        "\n%post\n" + reload.replace("#!/bin/sh\n", "") + "\n%postun\n" + reload.replace("#!/bin/sh\n", "") +
        "\n%files\n/opt/kmswitcher\n/usr/bin/kmswitcher\n/usr/bin/kmswitcher-gui\n"
        "/usr/share/applications/kmswitcher.desktop\n/usr/share/icons/hicolor/scalable/apps/kmswitcher.svg\n"
        "/usr/lib/udev/rules.d/71-kmswitcher-companion.rules\n", encoding="utf-8")
    subprocess.run(["rpmbuild", "-bb", "--define", f"_topdir {top}", "--define", "__os_install_post %{nil}",
                    "--define", "_build_id_links none", str(spec)], check=True)
    for path in (top / "RPMS").rglob("*.rpm"):
        # Keep the installed package identity stable so upgrades replace it.
        shutil.copy2(path, output / path.name.replace("kmswitcher-", "kmswitcher-companion-", 1))
    (output / "release.json").write_text(json.dumps(dict(identity, product="companion"), indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build/release/app/kmswitcher")
    parser.add_argument("--identity", type=Path, default=ROOT / "build/release/version.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/installers")
    parser.add_argument("--print-build-dependencies", action="store_true",
                        help="Print the Debian desktop libraries used by the Linux build container")
    args = parser.parse_args()
    if args.print_build_dependencies:
        print(" ".join(item.split()[0] for item in DEB_DEPENDENCIES))
        raise SystemExit()
    package(args.app.resolve(), json.loads(args.identity.read_text()), args.output.resolve())
