"""Run the actual Bash launcher with fake hardware-free process entry points."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class IsolationLauncherTests(unittest.TestCase):
    def test_three_bells_preserve_success_failure_and_inhibitor_exit_codes(self):
        bash = shutil.which("bash")
        if os.name == "nt": bash = "C:/Program Files/Git/bin/bash.exe"
        if not bash or not Path(bash).exists(): self.skipTest("Bash is unavailable")
        launcher = Path(__file__).resolve().parents[1] / "run-usb-isolation.sh"
        for inhibited, status, bells in ((False, 0, True), (False, 1, True), (False, 2, True), (False, 130, True), (True, 0, True), (True, 1, True), (False, 1, False)):
            with self.subTest(inhibited=inhibited, status=status, bells=bells), tempfile.TemporaryDirectory() as name:
                root = Path(name); (root / ".venv/bin").mkdir(parents=True); (root / "bin").mkdir()
                shutil.copyfile(launcher, root / launcher.name)
                (root / ".venv/bin/python").write_text("#!/usr/bin/env bash\nprintf 'runner-finished\\n'\nexit " + str(status) + "\n")
                (root / "bin/sleep").write_text("#!/usr/bin/env bash\nexit 0\n")
                inhibitor = "#!/usr/bin/env bash\n"
                if inhibited:
                    inhibitor += '[[ "$*" == *"Checking sleep inhibition"* ]] && exit 0\nwhile [[ "$1" == --* ]]; do shift; done\n"$@"\n'
                else: inhibitor += "exit 1\n"
                (root / "bin/systemd-inhibit").write_text(inhibitor)
                for p in [root / ".venv/bin/python", *list((root / "bin").iterdir())]: p.chmod(0o755)
                # Set PATH inside Bash so Windows drive letters are converted by Bash itself.
                script = 'export PATH="$PWD/bin:$PATH"; export KMS_COMPLETION_BELLS=' + ('1' if bells else '0') + '; bash run-usb-isolation.sh --seconds 1; result=$?; printf "parent-alive\\n"; exit "$result"'
                completed = subprocess.run([bash, "-c", script], cwd=root, capture_output=True)
                self.assertEqual(completed.returncode, status, completed.stderr.decode(errors="replace"))
                self.assertEqual(completed.stdout.count(b"\a"), 3 if bells else 0)
                self.assertTrue(completed.stdout.endswith(b"parent-alive\n"))
                if bells: self.assertIn(b"\a\a\aparent-alive", completed.stdout)
                self.assertIn(b"runner-finished", completed.stdout)
