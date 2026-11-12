"""Collect the source notices for a GitHub firmware bundle."""
import argparse
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
NOTICE_PREFIXES = ("LICENSE", "COPYING", "COPYRIGHT", "NOTICE")


def append_notices(source, label, stream):
    for path in sorted(source.rglob("*")):
        if path.is_file() and not path.is_symlink() and path.name.upper().startswith(NOTICE_PREFIXES):
            heading = f"\n\n===== {label}/{path.relative_to(source).as_posix()} =====\n"
            stream.write(heading.encode("utf-8"))
            stream.write(path.read_bytes())


def collect(output, sdk):
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    shutil.copy2(ROOT / "THIRD-PARTY-NOTICES.md", output / "THIRD-PARTY-NOTICES.md")
    notices = output / "third-party-notices"
    esp = notices / "esp-idf"
    esp.mkdir(parents=True, exist_ok=True)
    shutil.copy2(sdk / "LICENSE", esp / "LICENSE")
    shutil.copy2(sdk / "docs/en/COPYRIGHT.rst", esp / "COPYRIGHT.rst")
    # Keep component notices in one file so the bundle stays within the
    # updater's bounded ZIP entry count.
    with (notices / "component-notices.txt").open("wb") as stream:
        append_notices(sdk / "components", "ESP-IDF/components", stream)
        for project in ("firmware/controller", "firmware/receiver", "diagnostics/hid-generator"):
            append_notices(ROOT / project / "managed_components", project, stream)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    collect(args.output, Path(os.environ["IDF_PATH"]))
