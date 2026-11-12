"""Source-checkout entry point for shared receiver management."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "companion"))
from kms_update.management import main, ManagementError

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, ManagementError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
