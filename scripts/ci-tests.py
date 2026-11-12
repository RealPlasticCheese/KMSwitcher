"""Run offline checks, preserving logs and Azure-compatible results after failures."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SUITES = {
    "native": ["test-memory.py", "test-stream.py", "test-core.py", "test-desktop.py", "audit-routing.py", "test-link-receive.py", "test-text.py",
               "test-console-output.py", "test-keyboard-console.py", "test-switching-regressions.py", "test-receiver-recovery.py"],
    "sdk": ["test-usb-hotplug.py", "test-usb-allocation.py"],
}


def run(suite):
    output = ROOT / "build/test-results"; output.mkdir(parents=True, exist_ok=True)
    logs = ROOT / "build/test-logs"; logs.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, PYTHONPATH=os.pathsep.join(map(str, [ROOT / "companion", ROOT / "linux"])),
                       QT_QPA_PLATFORM="offscreen", PYTHONUTF8="1")
    if suite in SUITES:
        commands = [(name, [sys.executable, str(ROOT / "scripts" / name)] +
                     (["--skip-native-build"] if name == "audit-routing.py" else [])) for name in SUITES[suite]]
    elif suite in ("python", "companion", "setup", "stress", "packaging"):
        groups = {"companion": [("companion", "companion/tests")],
                  "setup": [("management", "tests/python"), ("packaging", "tests/release")],
                  "stress": [("stress", "linux/tests")],
                  "packaging": [("packaging", "tests/release")]}
        commands = [(name, [sys.executable, "-m", "xmlrunner", "discover", "-s", directory,
                            "-o", str(output / name)]) for name, directory in
                    groups.get(suite, [("companion", "companion/tests"), ("management", "tests/python"), ("packaging", "tests/release"), ("stress", "linux/tests")])]
    else:
        raise ValueError("Unknown suite")
    report = ET.Element("testsuite", name=suite, tests=str(len(commands)))
    failures = 0
    for name, command in commands:
        start = time.monotonic()
        print(f"Running {name}", flush=True)
        path = logs / (name + ".log")
        with path.open("w", encoding="utf-8") as stream:
            try:
                result = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124; stream.write("\nCheck exceeded ten minutes.\n")
        text = path.read_text(encoding="utf-8", errors="replace")
        case = ET.SubElement(report, "testcase", name=name, classname=suite, time=f"{time.monotonic() - start:.3f}")
        if code:
            failures += 1
            ET.SubElement(case, "failure", message=f"Exit code {code}; see {path.name}").text = text[-16000:]
            print(text[-16000:])
        else:
            print("PASS " + name)
        ET.SubElement(case, "system-out").text = text[-16000:]
    report.set("failures", str(failures))
    ET.ElementTree(report).write(output / f"{suite}-commands.xml", encoding="utf-8", xml_declaration=True)
    return bool(failures)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=("native", "sdk", "python", "companion", "setup", "stress", "packaging"))
    raise SystemExit(run(parser.parse_args().suite))
