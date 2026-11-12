"""Disk-backed clipboard snapshots and validated, bounded incoming bundles."""
import hashlib
from datetime import datetime
from itertools import count
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import zipfile

FORMAT_EXTENSIONS = {"image/png": ".png", "text/html": ".html", "text/rtf": ".rtf",
                     "application/rtf": ".rtf", "text/plain": ".txt"}
FORMATS = FORMAT_EXTENSIONS.keys()
MAX_ENTRIES = 10000
DEFAULT_LIMIT = 256 * 1024 * 1024
RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}


def safe_name(name):
    path = PurePosixPath(name)
    if (not name or "\\" in name or path.is_absolute() or str(path) != name or
            any(p in ("", ".", "..") or re.search(r'[<>:"|?*\x00-\x1f]', p) or
                p.endswith((".", " ")) or p.split(".")[0].upper() in RESERVED for p in path.parts)):
        raise ValueError("Clipboard bundle contains an unsafe file name")
    return path


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(256 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def build_bundle(path, formats, files=(), limit=DEFAULT_LIMIT, cancelled=lambda: False):
    """Freeze bytes before sending. Reject links/devices and files changed during capture."""
    path = Path(path)
    total, count, names = 0, 0, set()
    manifest = {"schema": 1, "kind": "files" if files else "text" if set(formats) == {"text/plain"} else "content",
                "formats": {}, "roots": []}
    def check(size, name):
        nonlocal total, count
        safe_name(name)
        folded = name.casefold()
        if folded in names:
            raise ValueError("Clipboard contains colliding file names")
        names.add(folded)
        total += size; count += 1
        if total > limit or count > MAX_ENTRIES or cancelled():
            raise ValueError("Clipboard snapshot cancelled or exceeds its size/file-count limit")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            for index, (mime, data) in enumerate(formats.items()):
                if mime not in FORMATS:
                    raise ValueError(f"Unsupported clipboard format: {mime}")
                entry = f"formats/{index}"
                check(len(data), entry)
                archive.writestr(entry, data)
                manifest["formats"][mime] = entry
            for source in files:
                source = Path(source)
                root = f"files/{source.name}"
                safe_name(root)
                manifest["roots"].append(root)
                def add(file, entry):
                    if cancelled():
                        raise ValueError("Clipboard snapshot cancelled")
                    before = file.lstat()
                    if stat.S_ISLNK(before.st_mode) or getattr(before, "st_file_attributes", 0) & 0x400:
                        raise ValueError("Symbolic links and junctions cannot be sent")
                    if stat.S_ISDIR(before.st_mode):
                        check(0, entry)
                        archive.writestr(entry + "/", b"")
                        for child in sorted(file.iterdir()):
                            add(child, entry + "/" + child.name)
                    elif stat.S_ISREG(before.st_mode):
                        check(before.st_size, entry)
                        copied = 0
                        with file.open("rb") as source, archive.open(entry, "w") as output:
                            for block in iter(lambda: source.read(256 * 1024), b""):
                                copied += len(block)
                                if copied > before.st_size or cancelled():
                                    raise ValueError("File grew during capture or clipboard snapshot was cancelled")
                                output.write(block)
                        after = file.stat()
                        if copied != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
                            raise ValueError(f"File changed while taking clipboard snapshot: {file.name}")
                    else:
                        raise ValueError("Only regular files and folders can be sent")
                add(source, root)
            if not formats and not files:
                raise ValueError("Clipboard has no supported text, image or local files")
            archive.writestr("manifest.json", json.dumps(manifest).encode())
        if path.stat().st_size > limit:
            raise ValueError("Clipboard bundle exceeds the transfer size limit")
        return manifest
    except Exception:
        path.unlink(missing_ok=True)
        raise


def unpack_bundle(path, destination, limit=DEFAULT_LIMIT):
    """Do not trust archive paths, declared sizes, MIME names or source-provided kinds."""
    destination = Path(destination)
    if destination.exists():
        raise ValueError("Clipboard staging directory already exists")
    try:
        with zipfile.ZipFile(path) as archive:
            entries = archive.infolist()
            if len(entries) > MAX_ENTRIES + 1 or sum(i.file_size for i in entries) > limit:
                raise ValueError("Incoming clipboard exceeds the extraction limit")
            names = set()
            for info in entries:
                name = info.filename.rstrip("/")
                safe_name(name)
                if name.casefold() in names or stat.S_ISLNK(info.external_attr >> 16) or info.flag_bits & 1:
                    raise ValueError("Incoming clipboard contains duplicate, linked or encrypted entries")
                names.add(name.casefold())
            info = archive.getinfo("manifest.json")
            if info.file_size > 65536:
                raise ValueError("Clipboard manifest is too large")
            manifest = json.loads(archive.read(info))
            if not isinstance(manifest, dict):
                raise ValueError("Invalid clipboard manifest")
            formats, roots = manifest.get("formats"), manifest.get("roots")
            if manifest.get("schema") != 1 or not isinstance(formats, dict) or not isinstance(roots, list):
                raise ValueError("Unsupported clipboard bundle")
            if not formats and not roots or len(roots) != len(set(roots)):
                raise ValueError("Empty or invalid clipboard bundle")
            for mime, entry in formats.items():
                if mime not in FORMATS or not isinstance(entry, str) or not entry.startswith("formats/"):
                    raise ValueError("Unsupported clipboard format")
                safe_name(entry)
                if archive.getinfo(entry).is_dir():
                    raise ValueError("Clipboard format must be a regular file")
            for root in roots:
                if not isinstance(root, str) or len(safe_name(root).parts) != 2 or not root.startswith("files/"):
                    raise ValueError("Invalid clipboard file root")
                if not any(i.filename.rstrip("/") == root for i in entries):
                    raise ValueError("Missing clipboard file root")
            staged_formats = {}
            staged_entries = {}
            for mime, extension in FORMAT_EXTENSIONS.items():
                if mime in formats:
                    entry = formats[mime]
                    name = "Clipboard-2.rtf" if mime == "application/rtf" and "text/rtf" in formats else f"Clipboard{extension}"
                    staged_formats[mime] = staged_entries.setdefault(entry, f"formats/{name}")
            for info in entries:
                name = info.filename.rstrip("/")
                if name == "manifest.json":
                    continue
                if name not in staged_entries and not any(name == root or name.startswith(root + "/") for root in roots):
                    raise ValueError("Unreferenced clipboard entry")
                target = destination / staged_entries.get(name, name)
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(info) as source, target.open("xb") as output:
                        shutil.copyfileobj(source, output, 256 * 1024)
            manifest["kind"] = "files" if roots else "text" if set(formats) == {"text/plain"} else "content"
            # Invalid Unicode must not poison automatic text delivery.
            for mime, entry in staged_formats.items():
                if mime in ("text/plain", "text/html"):
                    destination.joinpath(entry).read_text(encoding="utf-8")
            manifest["formats"] = staged_formats
            (destination / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            return manifest
    except Exception:
        if destination.exists():
            shutil.rmtree(destination)
        raise


def save_files(staging, manifest, parent, name):
    """Always create a new folder; never overwrite destination files."""
    safe_name(name)
    if len(PurePosixPath(name).parts) != 1:
        raise ValueError("Invalid incoming folder name")
    target = Path(parent) / name
    target.mkdir(exist_ok=False)
    try:
        for root in manifest["roots"]:
            source = Path(staging).joinpath(*safe_name(root).parts)
            if source.is_dir():
                shutil.copytree(source, target / source.name)
            else:
                shutil.copy2(source, target / source.name)
        return str(target)
    except Exception:
        shutil.rmtree(target)
        raise


def save_content(staging, manifest, parent):
    """Save all supported representations under a common, collision-safe stem."""
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    entries = {}
    for mime, extension in FORMAT_EXTENSIONS.items():
        if mime in manifest["formats"]:
            entries.setdefault(extension, manifest["formats"][mime])
    if not entries:
        raise ValueError("No supported clipboard content to save")

    staging, parent = Path(staging), Path(parent)
    for attempt in count():
        stem = stamp if attempt == 0 else f"{stamp}-{attempt}"
        created = []
        complete = False
        try:
            for extension, entry in entries.items():
                target = parent / (stem + extension)
                try:
                    output = target.open("xb")
                except FileExistsError:
                    break
                created.append(target)
                with output, (staging / entry).open("rb") as source:
                    shutil.copyfileobj(source, output, 256 * 1024)
            else:
                complete = True
                return str(created[0])
        finally:
            if not complete:
                for target in created:
                    target.unlink(missing_ok=True)
