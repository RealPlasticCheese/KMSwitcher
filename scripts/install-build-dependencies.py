"""Install hash-locked build tools before building the esptool source distribution."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def install(platform):
    lock = ROOT / "packaging" / f"requirements-{platform}.lock"
    content = lock.read_text()
    blocks = re.findall(r"(?m)^(?:setuptools|wheel)==[^\n]*(?:\n[ \t]+[^\n]*)*", content)
    if len(blocks) != 2:
        raise ValueError("Lock must pin and hash both setuptools and wheel")
    bootstrap = ROOT / "build/bootstrap-requirements.lock"
    bootstrap.parent.mkdir(parents=True, exist_ok=True)
    bootstrap.write_text("\n".join(blocks) + "\n")
    subprocess.run([sys.executable, "-m", "pip", "install", "--require-hashes", "-r", str(bootstrap)], check=True)
    subprocess.run([sys.executable, "-m", "pip", "install", "--no-build-isolation", "--require-hashes", "-r", str(lock)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("windows", "linux"), default="windows" if sys.platform == "win32" else "linux")
    install(parser.parse_args().platform)
