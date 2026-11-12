"""Check installer preparation without a frozen app, network or installer execution."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == "win32", "Windows installer preparation")
class InstallerPreflightTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "scripts").mkdir()
        self.script = self.root / "scripts/package-windows.ps1"
        shutil.copy2(ROOT / "scripts/package-windows.ps1", self.script)
        tools = self.root / "build/tools"
        (tools / "inno").mkdir(parents=True)
        (tools / "inno/ISCC.exe").touch()
        self.installer = tools / "innosetup-test-x64.exe"
        self.installer.write_bytes(b"checksum-checked test installer; never executed")
        (self.root / "packaging").mkdir()
        (self.root / "packaging/tools.json").write_text(json.dumps(dict(
            innoVersion="test", innoUrl="https://example.invalid/innosetup-test-x64.exe",
            innoSha256=hashlib.sha256(self.installer.read_bytes()).hexdigest())))

    def run_prepare(self, environment=None, prefix=""):
        launcher = self.root / "prepare.ps1"
        # Match the pipeline task's terminating-error behavior, and load the
        # Windows PowerShell utility module even when launched from PowerShell 7.
        launcher.write_text("$ErrorActionPreference = 'Stop'\n"
                            'Import-Module "$PSHOME/Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1"\n'
                            'Import-Module "$PSHOME/Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1"\n'
                            + prefix + "\n& $PSScriptRoot/scripts/package-windows.ps1 -PrepareOnly\n")
        clean = {key: value for key, value in os.environ.items() if not key.startswith("KMS_SIGNING_")}
        clean.update(environment or {})
        return subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(launcher)],
                              env=clean, capture_output=True, text=True, timeout=30)

    def test_prepare_requires_neither_frozen_app_nor_release_identity(self):
        result = self.run_prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS: installer compiler", result.stdout)
        self.assertFalse((self.root / "build/release").exists())

    def test_corrupt_replacement_installer_fails_before_freezing(self):
        self.installer.write_bytes(b"corrupt cache")
        result = self.run_prepare(prefix="""
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, [uri]$Uri, [string]$OutFile, [int]$TimeoutSec)
    [IO.File]::WriteAllText($OutFile, 'corrupt download')
}
""")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum failed", result.stderr)
        self.assertEqual(self.installer.read_bytes(), b"corrupt cache")

    def test_installer_setup_failure_is_reported(self):
        (self.root / "build/tools/inno/ISCC.exe").unlink()
        result = self.run_prepare(prefix="function Start-Process { [pscustomobject]@{ ExitCode = 1 } }")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("installation failed", result.stderr)

    def test_missing_or_unresolved_signing_password_fails_early(self):
        for password in ("", "$(windowsSigningPassword)"):
            with self.subTest(password=password):
                result = self.run_prepare(dict(KMS_SIGNING_PFX="certificate.pfx", KMS_SIGNING_PASSWORD=password))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Set windowsSigningPassword", result.stderr)

    def test_missing_signing_sdk_fails_before_freezing(self):
        result = self.run_prepare(dict(KMS_SIGNING_PFX="certificate.pfx", KMS_SIGNING_PASSWORD="test-password",
                                       **{"ProgramFiles(x86)": str(self.root / "empty-sdk")}),
                                  prefix="function Get-PfxData { }")
        self.assertNotEqual(result.returncode, 0)
        # Depending on PowerShell's glob resolution, the missing SDK is reported
        # by Get-ChildItem or by the explicit signing-tool check.
        self.assertTrue("Windows SDK signing tool is missing" in result.stderr or "Cannot find path" in result.stderr,
                        result.stderr)
