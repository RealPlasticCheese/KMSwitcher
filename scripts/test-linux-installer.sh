#!/bin/sh
# Run only in an isolated disposable CI container.
set -eu
format=$1
directory=$2
export QT_QPA_PLATFORM=xcb
export XDG_SESSION_TYPE=x11
export XDG_CONFIG_HOME=/tmp/kmswitcher-installer-smoke
mkdir -p "$XDG_CONFIG_HOME/KMSwitcher"
printf '%s' '{"enabled":false,"installer_test":"preserve"}' > "$XDG_CONFIG_HOME/KMSwitcher/preferences.json"
marker=$(sha256sum "$XDG_CONFIG_HOME/KMSwitcher/preferences.json")
for pass in 1 2; do
    if [ "$pass" = 2 ]; then
        mkdir -p "/opt/kmswitcher/firmware"
        printf legacy > "/opt/kmswitcher/firmware/obsolete.bin"
        printf legacy > "/opt/kmswitcher/KMSwitcher.html"
    fi
    if [ "$format" = deb ]; then
        apt-get update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -y "$directory"/*.deb
        if [ "$pass" = 2 ]; then DEBIAN_FRONTEND=noninteractive apt-get install --reinstall -y "$directory"/*.deb; fi
    else
        dnf install -y "$directory"/*.rpm
        if [ "$pass" = 2 ]; then dnf reinstall -y "$directory"/*.rpm; fi
    fi
    python3 scripts/test-packaged.py /opt/kmswitcher
    test "$(sha256sum "$XDG_CONFIG_HOME/KMSwitcher/preferences.json")" = "$marker"
done
if [ "$format" = deb ]; then
    apt-get remove -y kmswitcher
else
    # Keep the desktop dependencies for the subsequent tar installer check.
    dnf --setopt=clean_requirements_on_remove=False remove -y kmswitcher
fi
test ! -e /opt/kmswitcher/kmswitcher
test ! -e /usr/lib/udev/rules.d/71-kmswitcher-companion.rules
test "$(sha256sum "$XDG_CONFIG_HOME/KMSwitcher/preferences.json")" = "$marker"
echo 'PASS: native installation, reinstall/upgrade and uninstall preserve preferences.'

# Reuse the installed desktop dependencies to check the portable install path too.
portable=$(mktemp -d)
tar -xzf "$directory"/*.tar.gz -C "$portable"
set -- "$portable"/KMSwitcher-*
sh "$1/install.sh"
python3 scripts/test-packaged.py /opt/kmswitcher
test "$(sha256sum "$XDG_CONFIG_HOME/KMSwitcher/preferences.json")" = "$marker"
sh /opt/kmswitcher/uninstall-portable.sh
test ! -e /opt/kmswitcher/kmswitcher
test ! -e /usr/lib/udev/rules.d/71-kmswitcher-companion.rules
test "$(sha256sum "$XDG_CONFIG_HOME/KMSwitcher/preferences.json")" = "$marker"
echo 'PASS: portable installation and uninstall preserve preferences.'
