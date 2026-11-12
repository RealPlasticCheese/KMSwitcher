"""Check inexpensive CI prerequisites before tests, SDK builds or application freezing."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ("firmware", "companion", "setup", "stress", "web")


def command(arguments, **kwargs):
    print("Checking " + " ".join(map(str, arguments)), flush=True)
    subprocess.run(arguments, check=True, timeout=60, **kwargs)


def check_cache(path):
    path.mkdir(parents=True, exist_ok=True)
    # Test actual access under the agent identity, including restrictive files
    # restored from a container cache. os.access alone is insufficient as root.
    with tempfile.TemporaryFile(dir=path) as probe:
        probe.write(b"cache probe")
        probe.seek(0)
        if probe.read() != b"cache probe":
            raise RuntimeError("Cache read/write probe failed")
    def fail(error):
        raise error

    for directory, _, files in os.walk(path, onerror=fail):
        for name in files:
            with (Path(directory) / name).open("rb") as stream:
                stream.read(1)
    print(f"Cache is readable and writable: {path}", flush=True)


def check_source(component):
    settings = json.loads((ROOT / "pipelines/components.json").read_text())
    if component not in settings:
        raise ValueError("Unknown component: " + component)
    (ROOT / f"versions/{component}.txt").read_text()
    command(["git", "--version"])
    if component == "firmware":
        toolchain = json.loads((ROOT / "toolchain.json").read_text())
        if set(toolchain["targets"]) != {"esp32s2", "esp32s3"}:
            raise ValueError("The firmware toolchain must support both board targets")
    elif component != "web":
        platform = "windows" if sys.platform == "win32" else "linux"
        lock = ROOT / f"packaging/requirements-{platform}.lock"
        if not lock.is_file():
            raise FileNotFoundError("Missing hash-locked dependencies: " + str(lock))


def check_python(component):
    if sys.version_info < (3, 10):
        raise RuntimeError("CI requires Python 3.10 or newer")
    command([sys.executable, "-m", "pip", "check"])
    # Qt can abort the process when native libraries/plugins are missing, so
    # exercise it in a child process rather than relying on find_spec/imports.
    modules = ["yaml", "xmlrunner", "hid", "serial", "PyInstaller", "esptool"]
    if component == "stress":
        modules += ["evdev", "pyudev"]
    code = "\n".join("import " + module for module in modules)
    code += "\nfrom PySide6.QtWidgets import QApplication\napp = QApplication([])\n"
    command([sys.executable, "-c", code], env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
    if component in ("companion", "firmware"):
        from native_build import NativeCompiler
        compiler = NativeCompiler()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "probe.c"
            source.write_text("int kms_ci_probe(void) { return 0; }\n")
            obj = source.with_suffix(".obj")
            library = source.with_suffix(".dll")
            command(compiler.compile_command(source, obj))
            command(compiler.link_command(library, [obj], exports=["kms_ci_probe"]))
            command([sys.executable, "-c", "import ctypes, sys; sys.exit(ctypes.CDLL(sys.argv[1]).kms_ci_probe())", str(library)])


def check_package(component):
    # Setup and Stress write this value into release-source.json after freezing.
    # Reject missing and unresolved Azure macros before installing/freezing.
    if component in ("setup", "stress"):
        value = os.environ.get("KMS_FIRMWARE_PIPELINE_ID", "")
        if not value.isascii() or not value.isdecimal() or int(value) <= 0:
            raise ValueError("Set KMS_FIRMWARE_PIPELINE_ID to the Firmware pipeline's positive definition ID")
        for name in ("SYSTEM_COLLECTIONURI", "SYSTEM_TEAMPROJECTID"):
            value = os.environ.get(name, "")
            if not value or value.startswith("$("):
                raise ValueError("Missing Azure packaging variable: " + name)
    if sys.platform != "win32":
        if not shutil.which("docker"):
            raise RuntimeError("Linux packaging requires Docker")
        command(["docker", "info", "--format", "{{.ServerVersion}}"])


def check_sdk():
    command([sys.executable, "-c", "import esp_idf_size; from elftools.elf.elffile import ELFFile"])
    for name in ("cmake", "ninja", "xtensa-esp32s2-elf-gcc", "xtensa-esp32s3-elf-gcc"):
        command([name, "--version"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", choices=COMPONENTS, required=True)
    parser.add_argument("--phase", choices=("source", "python", "package", "sdk", "cache"), default="source")
    parser.add_argument("--cache", type=Path)
    args = parser.parse_args()
    if args.cache:
        check_cache(args.cache)
    if args.phase == "source":
        check_source(args.component)
    elif args.phase == "python":
        check_python(args.component)
    elif args.phase == "package":
        check_package(args.component)
    elif args.phase == "sdk":
        check_sdk()
    elif not args.cache:
        parser.error("--phase cache requires --cache")
    print("PASS: CI prerequisites", flush=True)


if __name__ == "__main__":
    main()
