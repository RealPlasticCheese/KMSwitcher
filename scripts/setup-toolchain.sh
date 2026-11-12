#!/usr/bin/env bash
# Source this script to install/activate the repository's pinned SDK on Linux.
set -euo pipefail

kms_repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
read -r kms_idf_version kms_idf_commit kms_idf_targets < <(python3 - "$kms_repo_root/toolchain.json" <<'PY'
import json, sys
settings = json.load(open(sys.argv[1]))
print(settings['espIdfVersion'], settings['espIdfCommit'], ','.join(settings['targets']))
PY
)
export IDF_PATH="${KMS_IDF_SOURCE_ROOT:-$kms_repo_root/.tools/esp}/$kms_idf_version/esp-idf"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$kms_repo_root/.tools/esp-idf-tools}"
if [[ ! -d "$IDF_PATH/.git" ]]; then
    mkdir -p -- "$(dirname -- "$IDF_PATH")"
    git clone --branch "$kms_idf_version" --depth 1 --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
if [[ $(git -C "$IDF_PATH" rev-parse HEAD) != "$kms_idf_commit" ]]; then
    printf 'SDK checkout differs from the pinned commit; move it aside and run setup again.\n' >&2
    return 1 2>/dev/null || exit 1
fi
git -C "$IDF_PATH" submodule update --init --recursive --depth 1

# Export validates both installed tools and Python dependencies. Repair a missing
# or incomplete cache using Espressif's installer, then activate the same SDK.
if ! (source "$IDF_PATH/export.sh" >/dev/null 2>&1 && python "$IDF_PATH/tools/idf.py" --version); then
    "$IDF_PATH/install.sh" "$kms_idf_targets"
fi
source "$IDF_PATH/export.sh"
