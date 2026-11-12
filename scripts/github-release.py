"""Stage verified GitHub release assets from one successful workflow run."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil

ARTIFACTS = {
    "windows-companion": Counter((".exe", ".zip")),
    "windows-setup": Counter((".exe", ".zip")),
    "linux-companion": Counter((".deb", ".rpm", ".gz")),
    "linux-setup": Counter((".deb", ".rpm", ".gz")),
    "linux-stress": Counter((".deb", ".rpm", ".gz")),
    "firmware": Counter((".zip",)),
    "stress-firmware": Counter((".zip",)),
    "web": Counter((".html",)),
    "esptool-source": Counter((".gz",)),
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def collect_assets(artifacts, commit):
    assets = {}
    firmware = []
    for name, extensions in ARTIFACTS.items():
        directory = artifacts / name
        manifest = read_json(directory / "component.json")
        if manifest.get("commit") != commit or manifest.get("stable") is not True:
            raise ValueError(f"{name} is not a stable build of {commit}")
        files = sorted(path for path in directory.iterdir() if path.is_file() and path.suffix in extensions)
        if Counter(path.suffix for path in files) != extensions:
            raise ValueError(f"{name} has incomplete or extra release assets")
        entries = {row["path"]: row for row in manifest["files"]}
        for path in files:
            expected = entries[path.name]
            if path.name in assets or not re.fullmatch(r"[A-Za-z0-9_.-]+", path.name):
                raise ValueError(f"Invalid or duplicate release asset {path.name}")
            if path.stat().st_size != expected["size"] or sha256(path) != expected["sha256"]:
                raise ValueError(f"Release asset checksum or size differs for {path.name}")
            assets[path.name] = path
        if name in ("firmware", "stress-firmware"):
            index = read_json(directory / "release-index.json")
            row, = index["releases"]
            product = "regular" if name == "firmware" else "stress"
            if (index.get("schema") != 1 or row.get("product") != product or row.get("commit") != commit
                    or row.get("stable") is not True or row.get("version") != manifest["version"]
                    or row.get("url") != files[0].name
                    or any(row.get(key) != entries[files[0].name][key] for key in ("sha256", "size"))):
                raise ValueError(f"Firmware index does not match {name}")
            firmware.append(row)
    return assets, firmware


def stage(artifacts, output, commit, repo, tag):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("Invalid GitHub repository")
    if not re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", tag):
        raise ValueError("Use a vMAJOR.MINOR.PATCH release tag")
    assets, firmware = collect_assets(artifacts, commit)
    output.mkdir(parents=True, exist_ok=False)
    for name, source in assets.items():
        shutil.copy2(source, output / name)
    base = f"https://github.com/{repo}/releases/download/{tag}/"
    releases = [dict(row, url=base + row["url"]) for row in firmware]
    (output / "release-index.json").write_text(
        json.dumps(dict(schema=1, releases=releases), indent=2) + "\n", encoding="utf-8")
    (output / "SHA256SUMS.txt").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(output.iterdir())), encoding="ascii")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    stage(args.artifacts, args.output, args.commit, args.repo, args.tag)
