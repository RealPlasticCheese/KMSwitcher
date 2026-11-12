"""Build paired firmware using an activated, pinned ESP-IDF environment."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = ("controller", "receiver", "board-check", "bringup", "led-sweep", "hid-generator", "usb-isolation")
ACTIONS = ("build", "reconfigure", "menuconfig", "size", "flash", "monitor")


def build(project, action="build", *, target=None, port=None, release=False,
          test_support=False, reset_config=False, root=ROOT):
    diagnostic = project not in ("controller", "receiver")
    if (release or test_support) and diagnostic:
        raise ValueError("Release/test profiles apply only to controller or receiver")
    if release and test_support:
        raise ValueError("Choose either the release or test profile")
    if action in ("flash", "monitor") and not port:
        raise ValueError("Specify --port explicitly for flash or monitor")
    if diagnostic:
        if project in ("hid-generator", "usb-isolation"):
            target = target or "esp32s3"
            if target != "esp32s3":
                raise ValueError(f"{project} requires esp32s3")
        if not target or (project == "led-sweep" and target != "esp32s2"):
            raise ValueError("Diagnostics require a matching --target (LED sweep requires esp32s2)")
    else:
        expected = "esp32s3" if project == "controller" else "esp32s2"
        if target and target != expected:
            raise ValueError("Target does not match the firmware role")
        target = expected
    idf = Path(os.environ.get("IDF_PATH", ""))
    if not os.environ.get("IDF_PATH") or not (idf / "tools/idf.py").is_file():
        raise ValueError("Activate the pinned ESP-IDF environment first")
    pinned = json.loads((root / "toolchain.json").read_text())["espIdfVersion"]
    actual = subprocess.check_output([sys.executable, str(idf / "tools/idf.py"), "--version"], text=True).strip()
    if actual != "ESP-IDF " + pinned:
        raise ValueError(f"Expected ESP-IDF {pinned}, found {actual}")
    if project == "controller" and action in ("build", "flash"):
        build("receiver", release=release, test_support=test_support, reset_config=reset_config, root=root)
    source = root / ("diagnostics" if diagnostic else "firmware") / project
    output = root / "build" / (f"{project}-{target}" if diagnostic else project + ("-test" if test_support else ""))
    if release:
        output = root / "build/production" / project
    config = (output if diagnostic or test_support or release else source) / "sdkconfig"
    if reset_config:
        config.unlink(missing_ok=True)
    command = [sys.executable, str(idf / "tools/idf.py"), "-C", str(source), "-B", str(output),
               "-D", f"IDF_TARGET={target}"]
    if project == "controller":
        receiver = root / "build" / ("production/receiver" if release else "receiver-test" if test_support else "receiver")
        command += ["-D", "KMS_RECEIVER_BUILD=" + receiver.as_posix()]
    if diagnostic or test_support or release:
        names = [f"sdkconfig.flash.{target}"]
        if diagnostic:
            profile = project if project != "board-check" else "diagnostic"
            names += [f"sdkconfig.{profile}.{target}"]
        else:
            names += ["sdkconfig.common", f"sdkconfig.{project}", "sdkconfig.release" if release else "sdkconfig.test"]
        command += ["-D", "SDKCONFIG=" + config.as_posix(), "-D",
                    "SDKCONFIG_DEFAULTS=" + ";".join((root / "config" / name).as_posix() for name in names)]
    if port:
        command += ["-p", port]
    subprocess.run([*command, action], check=True, cwd=root)
    if release and action == "build":
        for format_name, extension in (("text", "txt"), ("json2", "json")):
            report = subprocess.check_output([sys.executable, "-m", "esp_idf_size", "--format", format_name,
                                             str(output / f"kmswitcher_{project}.map")], text=True)
            (output / f"size.{extension}").write_text(report, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, choices=PROJECTS)
    parser.add_argument("--action", default="build", choices=ACTIONS)
    parser.add_argument("--target", choices=("esp32s2", "esp32s3"))
    parser.add_argument("--port")
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--test-support", action="store_true")
    parser.add_argument("--reset-config", action="store_true")
    build(**vars(parser.parse_args()))
