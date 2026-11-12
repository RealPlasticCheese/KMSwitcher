"""Bounded HTTPS release discovery and validated, offline-capable bundle import."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
from urllib.parse import urlencode, urljoin, urlsplit
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import zipfile

from . import config_directory
from .firmware import Bundle

MAX_DOWNLOAD = 128 * 1024 * 1024
PUBLIC_RELEASES = "https://github.com/RealPlasticCheese/KMSwitcher/releases"
PUBLIC_INDEX = PUBLIC_RELEASES + "/latest/download/release-index.json"


def https_url(url):
    parsed = urlsplit(url)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise ValueError("Use an HTTPS release address without embedded credentials")
    return url


def fetch(url, limit=MAX_DOWNLOAD):
    request = Request(https_url(url), headers={"User-Agent": "KMSwitcher-Updater", "Accept": "application/json, application/zip"})
    try:
        with urlopen(request, timeout=30) as response:
            https_url(response.geturl())
            if "text/html" in response.headers.get("Content-Type", ""):
                raise RuntimeError("This release source requires browser sign-in. Open the release in your browser, download the firmware ZIP, and import it here.")
            data = response.read(limit + 1)
    except HTTPError as error:
        if error.code in (401, 403):
            raise RuntimeError("This release source requires browser sign-in. Open the release in your browser, download the firmware ZIP, and import it here.") from error
        raise
    if len(data) > limit:
        raise ValueError("Release download exceeds the size limit")
    return data


def discover(source, product, *, development=False):
    """Read a published index or query successful builds in the configured Azure project."""
    if isinstance(source, str):
        document = json.loads(fetch(source, 1024 * 1024))
        if document.get("schema") != 1:
            raise ValueError("Unsupported release index")
        releases = [dict(row, url=https_url(urljoin(source, row["url"])))
                    for row in document["releases"] if row["product"] == product]
    else:
        base = https_url(source["azure"])
        filters = {"definitions": source["definition"], "resultFilter": "succeeded",
                   "statusFilter": "completed", "queryOrder": "finishTimeDescending", "$top": 100,
                   "api-version": "7.1"}
        tags = [] if development else ["kmswitcher-stable"]
        if product == "stress": tags.append("kmswitcher-stress-firmware")
        if tags: filters["tagFilters"] = ",".join(tags)
        query = urlencode(filters)
        builds = json.loads(fetch(base + "/_apis/build/builds?" + query, 1024 * 1024))["value"]
        releases = []
        for build in builds:
            ref = build["sourceBranch"]
            stable = ref.startswith(("refs/tags/firmware/v", "refs/tags/v"))
            if not stable and (not development or ref != "refs/heads/master"):
                continue
            artifact = "firmware" if product == "regular" else "stress-firmware"
            releases.append(dict(product=product, version=build["buildNumber"], commit=build["sourceVersion"],
                stable=stable, artifact=base + f"/_apis/build/builds/{build['id']}/artifacts?" +
                urlencode({"artifactName": artifact, "api-version": "7.1"}),
                page=build["_links"]["web"]["href"]))
    return [row for row in releases if development or row.get("stable") is True]


def install_archive(archive, product, *, expected=None, cache=None):
    """Reject traversal, links, expansion bombs, wrong roles and mixed-release files."""
    archive = Path(archive)
    if archive.stat().st_size > MAX_DOWNLOAD:
        raise ValueError("Firmware archive exceeds the size limit")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if expected and expected.get("sha256") and digest != expected["sha256"]:
        raise ValueError("Downloaded firmware archive checksum does not match the release index")
    cache = Path(cache) if cache else config_directory() / "firmware" / product
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        staging = Path(temporary)
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            if len(entries) > 256 or sum(item.file_size for item in entries) > MAX_DOWNLOAD:
                raise ValueError("Expanded firmware archive exceeds the size limit")
            names = set()
            for item in entries:
                path = PurePosixPath(item.filename)
                mode = item.external_attr >> 16
                if (path.is_absolute() or ".." in path.parts or "\\" in item.filename or
                        ":" in item.filename or stat.S_ISLNK(mode) or item.filename.casefold() in names):
                    raise ValueError("Unsafe or duplicate firmware archive path")
                names.add(item.filename.casefold())
                if not item.is_dir():
                    target = staging.joinpath(*path.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.open(item) as source, target.open("wb") as destination:
                        shutil.copyfileobj(source, destination)
        manifests = list(staging.rglob("bundle.json"))
        if len(manifests) != 1:
            raise ValueError("Choose an archive containing exactly one firmware bundle")
        bundle = Bundle(manifests[0].parent, product=product)
        identity = bundle.validate()
        for role in ("controller", "receiver"):
            _, metadata = bundle.application(role)
            if metadata["git"] != identity["commit"]:
                raise ValueError("Firmware application comes from another release")
        if expected and any(identity[key] != expected[key] for key in ("version", "commit")):
            raise ValueError("Downloaded firmware identity does not match the selected release")
        destination = cache / digest
        if not destination.exists():
            shutil.copytree(bundle.directory, destination)
        Bundle(destination, product=product).validate()
        return destination


def download(release, product):
    url = release.get("url")
    if not url:
        artifact = json.loads(fetch(release["artifact"], 1024 * 1024))
        url = artifact["resource"]["downloadUrl"]
    data = fetch(url)
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / "firmware.zip"
        archive.write_bytes(data)
        return install_archive(archive, product, expected=release)


def default_source():
    """Use the public firmware index unless the build configured a private source."""
    from .firmware import default_bundle
    path = default_bundle().parent / "release-source.json"
    return json.loads(path.read_text()) if path.exists() else PUBLIC_INDEX
