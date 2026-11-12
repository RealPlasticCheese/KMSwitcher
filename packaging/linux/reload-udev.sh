#!/bin/sh
set -eu
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules 2>/dev/null || true
fi
echo 'KMSwitcher: reconnect your receiver/programming cable, then open KMSwitcher from the application menu.'
