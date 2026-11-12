"""Exercise the same import and notice checks used by published firmware ZIPs."""
import json
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "companion"))
from kms_update.releases import install_archive


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        for product, component in (("regular", "firmware"), ("stress", "stress-firmware")):
            directory = ROOT / "build/release" / component
            row, = json.loads((directory / "release-index.json").read_text())["releases"]
            archive = directory / row["url"]
            install_archive(archive, product, expected=row, cache=Path(temporary) / product)
            with zipfile.ZipFile(archive) as package:
                for path in ("LICENSE", "THIRD-PARTY-NOTICES.md", "third-party-notices/esp-idf/LICENSE",
                             "third-party-notices/esp-idf/COPYRIGHT.rst", "third-party-notices/component-notices.txt"):
                    if not package.read(path).strip():
                        raise ValueError(f"{component} is missing notice content for {path}")
            print(f"PASS: {component} firmware imports with verified checksums and notices")
