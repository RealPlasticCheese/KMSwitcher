#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -x .venv/bin/python ]]; then
    echo 'Run ./install.sh once before starting KMSwitcher Test.'
    exit 1
fi
if command -v systemd-inhibit >/dev/null 2>&1 && \
    systemd-inhibit --what=sleep:idle --who=KMSwitcher --why='Checking sleep inhibition' true >/dev/null 2>&1; then
    exec systemd-inhibit --what=sleep:idle --who=KMSwitcher --why='Hardware test controller is open' \
        .venv/bin/python -m kms_stress "$@"
fi
echo 'Automatic sleep inhibition is unavailable; keep this laptop awake during tests.' >&2
exec .venv/bin/python -m kms_stress "$@"
