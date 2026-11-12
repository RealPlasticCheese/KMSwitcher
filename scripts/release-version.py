"""Calculate a component identity shared by its platform packages and artifacts."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def identity(base, ref, build_id, commit, component=None, *, stable=False):
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", base):
        raise ValueError("VERSION must contain MAJOR.MINOR.PATCH")
    if any(int(part) > 65535 for part in base.split(".")):
        raise ValueError("Version exceeds Windows file-version limits")
    tagged = ref.startswith("refs/tags/")
    tag = "refs/tags/" + ((component + "/v") if component else "v") + base
    if tagged and ref != tag:
        raise ValueError("Release tag must match the checked-in VERSION exactly")
    if not str(build_id).isdigit() or int(build_id) < 0:
        raise ValueError("Build ID must be a non-negative integer")
    stable = stable or tagged
    version = base if stable else f"{base}-dev.{build_id}"
    return dict(schema=1, version=version, python_version=base if stable else f"{base}.dev{build_id}",
                deb_version=base if stable else f"{base}~dev.{build_id}", rpm_version=base,
                rpm_release="1" if stable else f"0.dev.{build_id}", windows_version=base + ".0",
                commit=commit, build_id=str(build_id), stable=stable,
                input_protocol=2, management_protocol=1, desktop_protocol=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/version.json")
    parser.add_argument("--azure", action="store_true")
    parser.add_argument("--component", choices=("firmware", "companion", "setup", "web", "stress"))
    args = parser.parse_args()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    ref = os.environ.get("BUILD_SOURCEBRANCH", "local")
    version_path = ROOT / ("versions/" + args.component + ".txt") if args.component else ROOT / "companion/kms_companion/VERSION"
    release_tag = os.environ.get("KMS_GITHUB_RELEASE_TAG", "")
    if release_tag and (os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
                        or os.environ.get("GITHUB_REF") != "refs/heads/main"
                        or not re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", release_tag)):
        parser.error("Publish a vMAJOR.MINOR.PATCH release by running the GitHub workflow on main")
    build_id = os.environ.get("BUILD_BUILDID", os.environ.get("GITHUB_RUN_NUMBER", "0"))
    value = identity(version_path.read_text().strip(), ref, build_id, commit, args.component, stable=bool(release_tag))
    if args.component: value["component"] = args.component
    if value["stable"]:
        # A stable tag may only release a commit already merged to the default branch.
        subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", "origin/main"], cwd=ROOT, check=True)
        if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
            raise ValueError("Stable releases require a clean checkout")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(value))
    if args.azure:
        print(f"##vso[build.updatebuildnumber]{value['version']}")
        print(f"##vso[task.setvariable variable=releaseVersion;isOutput=true]{value['version']}")
        print(f"##vso[task.setvariable variable=stable;isOutput=true]{str(value['stable']).lower()}")


if __name__ == "__main__":
    main()
