#!/bin/sh
# Package lifecycle only: stop processes holding this installation's executables.
set -eu
if command -v fuser >/dev/null 2>&1; then
    for binary in /opt/kmswitcher/kmswitcher /opt/kmswitcher/kmswitcher-gui; do
        if [ -f "$binary" ]; then
            fuser -k -TERM "$binary" 2>/dev/null || true
            attempts=0
            while fuser "$binary" >/dev/null 2>&1; do
                attempts=$((attempts + 1))
                if [ "$attempts" -ge 10 ]; then
                    echo 'Please close KMSwitcher before installing or removing it.' >&2
                    exit 1
                fi
                sleep 1
            done
        fi
    done
fi
