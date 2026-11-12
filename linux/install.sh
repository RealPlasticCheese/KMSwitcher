#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
web_only=false
if [[ $# -gt 1 || ( $# -eq 1 && $1 != --web ) ]]; then
    echo 'Usage: bash install.sh [--web]' >&2
    exit 2
fi
if [[ ${1:-} == --web ]]; then web_only=true; fi
if command -v apt-get >/dev/null 2>&1; then
    echo 'Installing Python/build prerequisites (Debian/Ubuntu).'
    packages=(python3 python3-venv python3-dev build-essential)
    if [[ $web_only == false ]]; then packages+=(python3-tk); fi
    sudo apt-get install -y "${packages[@]}"
fi
if ! python3 -c 'import venv' >/dev/null 2>&1; then
    echo 'Python 3.10+, venv, development headers and a C compiler are required.'
    exit 1
fi
if [[ $web_only == false ]] && ! python3 -c 'import tkinter' >/dev/null 2>&1; then
    echo 'Install your distribution’s Python tkinter package, or use bash install.sh --web for browser-only use.'
    exit 1
fi
python3 -m venv .venv
.venv/bin/python -m pip install .
# Limit access to the test receiver and explicitly connected test UART bridges.
# The installer never adds the user to the unrestricted Linux input group.
rules=$(mktemp)
trap 'rm -f -- "$rules"' EXIT
cat > "$rules" <<'RULES'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4005", TAG+="uaccess"
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4005", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4006", TAG+="uaccess"
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4006", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", TAG+="uaccess"
RULES
echo 'Installing local device-permission rules (one administrator step).'
sudo install -m 644 "$rules" /etc/udev/rules.d/70-kmswitcher-stress.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=input
sudo udevadm trigger --subsystem-match=tty
if [[ $web_only == false ]]; then
.venv/bin/python - <<'PY'
from pathlib import Path
script = str(Path.cwd() / 'run.sh').replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$').replace('%', '%%')
destination = Path.home() / '.local/share/applications/kmswitcher-stress.desktop'
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text('[Desktop Entry]\nType=Application\nName=KMSwitcher Test\n'
                       f'Exec=/usr/bin/env bash "{script}"\nIcon=input-keyboard\nTerminal=false\nCategories=Development;\n')
PY
fi
echo 'Ready. Run bash runweb.sh for the browser dashboard, or bash run.sh for the desktop dashboard (requires Tk).'
echo 'Reconnect test devices if permissions have not refreshed.'
