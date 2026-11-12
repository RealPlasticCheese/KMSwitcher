#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install_dir="${XDG_DATA_HOME:-$HOME/.local/share}/kmswitcher"
python3 -m venv "$install_dir/venv"
"$install_dir/venv/bin/python" -m pip install "$base[ui]"
mkdir -p "$HOME/.local/bin" "${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ln -sf "$install_dir/venv/bin/kmswitcher" "$HOME/.local/bin/kmswitcher"
printf '[Desktop Entry]\nType=Application\nName=KMSwitcher\nExec="%s" ui\nTerminal=false\nCategories=Utility;\n' "$install_dir/venv/bin/kmswitcher" > "${XDG_DATA_HOME:-$HOME/.local/share}/applications/kmswitcher.desktop"
printf '%s\n' 'Installed. Install 70-kmswitcher.rules into /etc/udev/rules.d/ with administrator privileges, reload udev rules and reconnect the receiver.'
printf '%s\n' 'On Mint/Ubuntu, prerequisite packages are python3-venv, libxcb-cursor0 and x11-xserver-utils.'
