"""Desktop integration for KMSwitcher USB receivers."""
import json
from pathlib import Path

_directory = Path(__file__).parent
_build_file = _directory / "build-info.json"
BUILD_INFO = json.loads(_build_file.read_text(encoding="utf-8")) if _build_file.exists() else {}
_source_version = _directory.parents[1] / "versions/companion.txt"
if not _source_version.exists():
    _source_version = _directory / "VERSION"
__version__ = BUILD_INFO.get("version") or _source_version.read_text(encoding="utf-8").strip()
