"""Assemble checked release artifacts and retain successful stable Azure runs."""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import html
import urllib.request
import urllib.error
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]


def assemble(inputs, output, identity):
    inputs, output = inputs.resolve(), output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("Release output must be empty to prevent stale downloads")
    output.mkdir(parents=True, exist_ok=True)
    groups = ("windows-companion", "linux-companion", "firmware", "console", "windows-firmware-setup", "linux-firmware-setup", "linux-stress", "stress-firmware")
    for group in groups:
        source = inputs / group
        if not source.is_dir() or not any(source.rglob("*")):
            raise ValueError("Missing release input: " + group)
        shutil.copytree(source, output / group, dirs_exist_ok=True)
    for group, pattern in (("windows-companion", "*-setup.exe"), ("windows-companion", "*-portable.zip"),
                           ("linux-companion", "*.deb"), ("linux-companion", "*.rpm"), ("linux-companion", "*.tar.gz"), ("console", "KMSwitcher.html")):
        if not list((output / group).glob(pattern)):
            raise ValueError("Release is missing a required download: " + group + "/" + pattern)
    for group, patterns in (("windows-firmware-setup", ("*-setup.exe", "*-portable.zip")),
                            ("linux-firmware-setup", ("*.deb", "*.rpm", "*.tar.gz")),
                            ("linux-stress", ("*.deb", "*.rpm", "*.tar.gz"))):
        for pattern in patterns:
            if not list((output / group).glob(pattern)):
                raise ValueError("Release is missing a required download: " + group + "/" + pattern)
    import sys
    sys.path.insert(0, str(ROOT / "companion"))
    from kms_companion.firmware import Bundle
    Bundle(output / "firmware", product="regular").validate()
    stress = Bundle(output / "stress-firmware", product="stress").validate()
    if any(stress[key] != identity[key] for key in ("version", "commit")):
        raise ValueError("Test firmware identity mismatch")
    bundle = json.loads((output / "firmware/bundle.json").read_text())
    if bundle["commit"] != identity["commit"] or bundle["version"] != identity["version"]:
        raise ValueError("Release firmware identity mismatch")
    for group in ("windows-companion", "linux-companion", "windows-firmware-setup", "linux-firmware-setup", "linux-stress"):
        packaged = json.loads((output / group / "release.json").read_text(encoding="utf-8-sig"))
        expected_product = "stress" if group == "linux-stress" else "setup" if group.endswith("-setup") else "companion"
        if packaged.get("product") != expected_product:
            raise ValueError("Installer product mismatch: " + group)
        if packaged["commit"] != identity["commit"] or packaged["version"] != identity["version"]:
            raise ValueError("Installer identity mismatch: " + group)
    shutil.copy2(ROOT / "docs/installing.md", output / "START-HERE.md")
    updates = output / "firmware-updates"; updates.mkdir()
    index = dict(schema=1, releases=[])
    for product, folder in (("regular", "firmware"), ("stress", "stress-firmware")):
        archive = updates / f"KMSwitcher-{product}-firmware-{identity['version']}"
        path = Path(shutil.make_archive(str(archive), "zip", output / folder))
        index["releases"].append(dict(product=product, version=identity["version"], commit=identity["commit"],
            stable=identity.get("stable", False), url=path.relative_to(output).as_posix(),
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(), size=path.stat().st_size))
    (output / "release-index.json").write_text(json.dumps(index, indent=2) + "\n")
    downloads = [("Firmware Setup — Windows", "windows-firmware-setup"), ("Firmware Setup — Linux", "linux-firmware-setup"),
                 ("Optional Companion — Windows", "windows-companion"), ("Optional Companion — Linux", "linux-companion"),
                 ("Separate Stress Toolkit — Linux", "linux-stress"), ("Firmware-only update ZIPs", "firmware-updates")]
    sections, markdown = [], [f"# KMSwitcher {identity['version']} downloads", "", "Start with Firmware Setup. The screen-edge Companion is optional. Stress testing has a separate Linux toolkit.", ""]
    base = os.environ.get("SYSTEM_COLLECTIONURI", "") + os.environ.get("SYSTEM_TEAMPROJECTID", "")
    build_id = os.environ.get("BUILD_BUILDID")
    for title, group in downloads:
        paths = [p for p in sorted((output / group).iterdir()) if p.suffix in (".exe", ".zip", ".deb", ".rpm", ".gz")]
        links = "".join(f'<li><a href="{html.escape(p.relative_to(output).as_posix(), quote=True)}">{html.escape(p.name)}</a></li>' for p in paths)
        sections.append(f"<section><h2>{html.escape(title)}</h2><ul>{links}</ul></section>")
        if build_id and group != "firmware-updates":
            url = base + f"/_apis/build/builds/{build_id}/artifacts?" + urllib.parse.urlencode(dict(artifactName=group, **{"api-version": "7.1", "$format": "zip"}))
            markdown.append(f"- [{title}]({url})")
        else: markdown.append(f"- {title}: `{group}/`")
    (output / "DOWNLOADS.md").write_text("\n".join(markdown) + "\n", encoding="utf-8")
    (output / "START-HERE.html").write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>KMSwitcher downloads</title><style>body{font:17px system-ui;max-width:980px;margin:40px auto;padding:20px;background:#f3f6fa;color:#15253a}section{background:white;padding:15px 25px;margin:20px 0;border-radius:12px}li{margin:12px 0}a{color:#0759ae}</style>' +
        f'<h1>KMSwitcher {html.escape(identity["version"])}</h1><p>Extract the complete release before opening an installer. Start with <b>Firmware Setup</b> to flash or update devices. No Companion installation is required.</p><p>Windows: run the Firmware Setup installer, or extract a portable ZIP and open kms-setup.exe. Linux: open the DEB/RPM with your package manager. Use the tar archive installer on supported systems without those package managers.</p><p>S2 dongles support direct USB and OTA through the controller. Choose the method inside the tool. The separate Linux Stress Toolkit includes its own firmware updater and test dashboard.</p>' + "".join(sections), encoding="utf-8")
    files = []
    for path in sorted(output.rglob("*")):
        if path.is_file():
            files.append(dict(path=path.relative_to(output).as_posix(), size=path.stat().st_size,
                              sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    (output / "release-manifest.json").write_text(json.dumps(dict(identity, files=files), indent=2) + "\n")
    checksum_paths = [output / item["path"] for item in files] + [output / "release-manifest.json"]
    (output / "SHA256SUMS").write_text("".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output).as_posix()}\n" for p in checksum_paths))


def retention(probe=False, *, run_id=None, definition_id=None):
    url = os.environ["SYSTEM_COLLECTIONURI"] + os.environ["SYSTEM_TEAMPROJECTID"] + "/_apis/build/retention/leases"
    headers = {"Authorization": "Bearer " + os.environ["SYSTEM_ACCESSTOKEN"], "Content-Type": "application/json"}

    def request(method, data=None, **parameters):
        query = urllib.parse.urlencode(dict(parameters, **{"api-version": "7.1"}))
        body = json.dumps(data).encode() if data is not None else None
        operation = urllib.request.Request(url + "?" + query, data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(operation, timeout=30) as response:
                content = response.read()
                return json.loads(content) if content else None
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            try:
                detail = json.loads(detail).get("message", detail)
            except ValueError:
                pass
            raise RuntimeError(f"Azure retention request failed (HTTP {error.code}): {detail}") from None

    # Azure displays leases longer than 100 years as "forever". A probe uses
    # only one day and deletes exactly its own lease, preserving existing ones.
    run_id = int(run_id or os.environ["BUILD_BUILDID"])
    definition_id = int(definition_id or os.environ["SYSTEM_DEFINITIONID"])
    result = request("POST", [dict(daysValid=1 if probe else 365000,
        definitionId=definition_id, runId=run_id,
        ownerId="User:" + os.environ["BUILD_REQUESTEDFORID"], protectPipeline=False)])
    leases = result.get("value", []) if isinstance(result, dict) else result
    if not leases or len(leases) != 1 or not leases[0].get("leaseId"):
        raise RuntimeError("Azure did not create the requested retention lease")
    lease = leases[0]
    try:
        expiry = datetime.fromisoformat(lease["validUntil"].replace("Z", "+00:00"))
        minimum = datetime.now(timezone.utc) + timedelta(days=0 if probe else 36500)
        if lease["runId"] != run_id or lease["definitionId"] != definition_id or expiry <= minimum:
            raise RuntimeError("Azure retention lease does not protect the requested run")
    finally:
        if probe:
            request("DELETE", ids=lease["leaseId"])
    print("Release retention permission verified" if probe else "Stable release retained with an Azure forever lease")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--retain", action="store_true")
    parser.add_argument("--retention-probe", action="store_true")
    args = parser.parse_args()
    if args.retain or args.retention_probe:
        retention(args.retention_probe)
    else:
        assemble(args.inputs, args.output, json.loads(args.identity.read_text()))
