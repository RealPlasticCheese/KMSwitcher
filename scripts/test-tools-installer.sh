#!/bin/sh
# Disposable CI containers only: test standalone setup and separate stress packages.
set -eu
format=$1
directory=$2
product=$3
name=kmswitcher-$product
export QT_QPA_PLATFORM=xcb
export XDG_SESSION_TYPE=x11
export XDG_CONFIG_HOME=/tmp/kmswitcher-tools-installer-smoke
mkdir -p "$XDG_CONFIG_HOME/KMSwitcher" "$HOME/.config/kmswitcher-stress"
printf '%s' '{"installer_test":"preserve"}' > "$HOME/.config/kmswitcher-stress/profile.json"
marker=$(sha256sum "$HOME/.config/kmswitcher-stress/profile.json")
for pass in 1 2; do
    if [ "$pass" = 2 ]; then
        mkdir -p "/opt/$name/firmware"
        printf legacy > "/opt/$name/firmware/obsolete.bin"
        printf legacy > "/opt/$name/KMSwitcher.html"
    fi
    if [ "$format" = deb ]; then
        apt-get install -y "$directory"/*.deb
        if [ "$pass" = 2 ]; then apt-get install --reinstall -y "$directory"/*.deb; fi
    else
        dnf install -y "$directory"/*.rpm
        if [ "$pass" = 2 ]; then dnf reinstall -y "$directory"/*.rpm; fi
    fi
    python3 scripts/test-packaged.py "/opt/$name"
    test "$(sha256sum "$HOME/.config/kmswitcher-stress/profile.json")" = "$marker"
done
if [ "$format" = deb ]; then apt-get remove -y "$name"
else dnf --setopt=clean_requirements_on_remove=False remove -y "$name"; fi
test ! -e "/opt/$name/kms-setup"
portable=$(mktemp -d)
tar -xzf "$directory"/*.tar.gz -C "$portable"
sh "$portable/$name/install.sh"
python3 scripts/test-packaged.py "/opt/$name"
sh "/opt/$name/uninstall-portable.sh"
test ! -e "/opt/$name/kms-setup"
test "$(sha256sum "$HOME/.config/kmswitcher-stress/profile.json")" = "$marker"
echo "PASS: $product install, reinstall and removal preserve profiles."
