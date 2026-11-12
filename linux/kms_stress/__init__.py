"""Hardware-independent model plus Linux adapters for KMSwitcher testing."""

import json
from pathlib import Path

_identity = Path(__file__).with_name("build-info.json")
__version__ = json.loads(_identity.read_text())["version"] if _identity.exists() else "0.3.19"
