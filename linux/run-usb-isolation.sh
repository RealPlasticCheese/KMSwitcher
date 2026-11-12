#!/usr/bin/env bash
set -euo pipefail
completion_bells() {
    # EXIT traps retain the original status; do not exit from this handler.
    if [[ "${KMS_COMPLETION_BELLS:-1}" == 0 ]]; then return; fi
    printf '\a'
    sleep 0.25 || true
    printf '\a'
    sleep 0.25 || true
    printf '\a'
}
trap completion_bells EXIT
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -x .venv/bin/python ]]; then
    echo 'Run bash install.sh --web once before starting USB isolation.' >&2
    exit 1
fi
if command -v systemd-inhibit >/dev/null 2>&1 && \
    systemd-inhibit --what=sleep:idle --who=KMSwitcher --why='Checking sleep inhibition' true >/dev/null 2>&1; then
    systemd-inhibit --what=sleep:idle --who=KMSwitcher --why='USB-only hardware test' \
        .venv/bin/python -m kms_stress.usb_isolation "$@"
else
    echo 'Automatic sleep inhibition is unavailable; keep this test laptop awake.' >&2
    .venv/bin/python -m kms_stress.usb_isolation "$@"
fi
