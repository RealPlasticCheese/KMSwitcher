"""Exercise the production cache ownership commands with real Linux permissions."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux cache ownership")
class ContainerCacheTests(unittest.TestCase):
    def test_cache_is_enabled_for_root_and_readable_by_agent_after_success_or_failure(self):
        if os.getuid() == 0:
            self.skipTest("Requires a non-root agent to verify returned ownership")
        if not shutil.which("sudo") or subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode:
            self.skipTest("Requires the hosted Linux agent's passwordless sudo")
        template = (ROOT / "pipelines/templates/application.yml").read_text()
        commands = [line.strip() for line in template.splitlines()
                    if line.strip().startswith(("trap 'chown", "chown -R 0:0"))]
        self.assertEqual(len(commands), 2)
        for exit_code in (0, 1):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                cache = Path(directory) / "cache"
                cache.mkdir()
                workspace = Path(directory) / "source-build"
                workspace.mkdir()
                # Use the same ownership transition and EXIT trap as Docker,
                # without downloading an image or installing any dependencies.
                script = "\n".join(command.replace("/pip-cache", '"$KMS_CACHE_PATH"').replace("/src/build", '"$KMS_WORKSPACE_PATH"') for command in commands)
                probe = ("from pathlib import Path; import os; "
                         "from pip._internal.utils.filesystem import check_path_owner; "
                         "import sys; p=Path(sys.argv[1]); assert check_path_owner(str(p)); "
                         "f=p/'private-entry'; f.write_bytes(b'cached wheel'); f.chmod(0o600)")
                script += f"\n{shlex.quote(sys.executable)} -c {shlex.quote(probe)} {shlex.quote(str(cache))}\nexit {exit_code}\n"
                result = subprocess.run(["sudo", "-n", "env", f"KMS_CACHE_OWNER={os.getuid()}:{os.getgid()}",
                                         f"KMS_CACHE_PATH={cache}", f"KMS_WORKSPACE_PATH={workspace}",
                                         "bash", "-ec", script], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, exit_code, result.stderr)
                entry = cache / "private-entry"
                self.assertEqual(entry.read_bytes(), b"cached wheel")
                self.assertEqual(entry.stat().st_uid, os.getuid())
                self.assertEqual(cache.stat().st_uid, os.getuid())
