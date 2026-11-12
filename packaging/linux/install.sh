#!/bin/sh
# The portable archive uses the same payload as the native packages.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$(id -u)" -ne 0 ]; then
    if command -v pkexec >/dev/null 2>&1; then exec pkexec "$base/install.sh"; fi
    echo "Run: sudo sh '$base/install.sh'" >&2
    exit 1
fi
sh "$base/stop-installed.sh"
rm -rf -- /opt/kmswitcher/firmware
rm -f -- /opt/kmswitcher/KMSwitcher.html /opt/kmswitcher/kms-firmware /opt/kmswitcher/kms-setup
cp -a "$base/payload/." /
sh "$base/reload-udev.sh"
