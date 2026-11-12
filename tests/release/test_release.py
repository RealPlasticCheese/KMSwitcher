import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import hashlib
import io
import os
import subprocess
import urllib.error
from unittest.mock import patch
from test_firmware import make_bundle
from test_maintenance import make_release

ROOT = Path(__file__).resolve().parents[2]


def script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class ReleaseTests(unittest.TestCase):
    def retention_environment(self):
        return dict(SYSTEM_COLLECTIONURI="https://dev.azure.com/example/", SYSTEM_TEAMPROJECTID="project",
                    SYSTEM_ACCESSTOKEN="test-token", SYSTEM_DEFINITIONID="9", BUILD_BUILDID="123",
                    BUILD_REQUESTEDFORID="person")

    def retention_response(self):
        return io.BytesIO(json.dumps(dict(value=[dict(leaseId=42, runId=123, definitionId=9,
            validUntil="9999-12-31T00:00:00Z")])).encode())

    def test_retention_probe_creates_and_deletes_only_its_new_lease(self):
        with patch.dict(os.environ, self.retention_environment()), \
             patch("urllib.request.urlopen", side_effect=[self.retention_response(), io.BytesIO(b"")]) as call:
            script("publish-release").retention(probe=True)
        create, remove = [item.args[0] for item in call.call_args_list]
        self.assertEqual(create.method, "POST")
        self.assertIn("/_apis/build/retention/leases?", create.full_url)
        self.assertEqual(json.loads(create.data), [dict(daysValid=1, definitionId=9, runId=123,
            ownerId="User:person", protectPipeline=False)])
        self.assertEqual(remove.method, "DELETE")
        self.assertIn("ids=42", remove.full_url)

    def test_stable_retention_uses_a_forever_lease_without_deleting_it(self):
        with patch.dict(os.environ, self.retention_environment()), \
             patch("urllib.request.urlopen", return_value=self.retention_response()) as call:
            script("publish-release").retention()
        call.assert_called_once()
        self.assertGreater(json.loads(call.call_args.args[0].data)[0]["daysValid"], 36500)

    def test_retention_errors_explain_the_azure_failure(self):
        error = urllib.error.HTTPError("https://dev.azure.com/example", 400, "Bad Request", None,
                                      io.BytesIO(b'{"message":"A retention lease is required"}'))
        with patch.dict(os.environ, self.retention_environment()), \
             patch("urllib.request.urlopen", side_effect=error) as call:
            with self.assertRaisesRegex(RuntimeError, "HTTP 400.*retention lease is required"):
                script("publish-release").retention(probe=True)
        call.assert_called_once()

    def test_release_tag_must_match_single_version_source(self):
        identity = script("release-version").identity
        self.assertTrue(identity("1.2.3", "refs/tags/v1.2.3", "123456", "abc")["stable"])
        with self.assertRaises(ValueError): identity("1.2.3", "refs/tags/v1.2.4", "123", "abc")
        with self.assertRaises(ValueError): identity("1.2.3", "refs/tags/v1.2.3-rc.1", "123", "abc")
        self.assertEqual(identity("1.2.3", "refs/pull/1/merge", "123456", "abc")["deb_version"], "1.2.3~dev.123456")

    def test_preview_is_ordered_before_stable_in_python(self):
        from packaging.version import Version
        identity = script("release-version").identity
        preview = identity("1.2.3", "master", "90000", "abc")
        stable = identity("1.2.3", "refs/tags/v1.2.3", "90001", "abc")
        self.assertLess(Version(preview["python_version"]), Version(stable["python_version"]))

    def test_stable_release_rejects_unmerged_and_dirty_git_checkouts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*arguments):
                return subprocess.check_output(["git", "-C", str(root), *arguments], stderr=subprocess.STDOUT)
            git("init", "-b", "master")
            git("config", "user.name", "Release validation")
            git("config", "user.email", "release-tests@example.invalid")
            (root / "companion/kms_companion").mkdir(parents=True)
            (root / "companion/kms_companion/VERSION").write_text("1.2.3\n")
            (root / ".gitignore").write_text("build/\n")
            git("add", "."); git("commit", "-m", "Base release")
            git("update-ref", "refs/remotes/origin/main", "HEAD")
            module = script("release-version"); module.ROOT = root
            with patch.dict(os.environ, BUILD_SOURCEBRANCH="refs/tags/v1.2.3", BUILD_BUILDID="123"), \
                 patch("sys.argv", ["release-version", "--output", str(root / "build/version.json")]):
                module.main()
                self.assertTrue(json.loads((root / "build/version.json").read_text())["stable"])
                (root / "change.txt").write_text("feature")
                git("add", "."); git("commit", "-m", "Unmerged feature")
                with self.assertRaises(subprocess.CalledProcessError): module.main()
                git("update-ref", "refs/remotes/origin/main", "HEAD")
                (root / "dirty.txt").write_text("unsaved change")
                with self.assertRaisesRegex(ValueError, "clean checkout"): module.main()

    def test_incomplete_download_set_is_never_published(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, "Missing release input"):
                script("publish-release").assemble(root / "inputs", root / "output", {})

    def test_complete_release_records_the_hash_of_every_download(self):
        # Relative paths require the fixture and working directory on the same drive.
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as directory:
            root = Path(directory); inputs = root / "inputs"
            for group in ("windows-companion", "linux-companion", "firmware", "console", "windows-firmware-setup", "linux-firmware-setup", "linux-stress", "stress-firmware"):
                (inputs / group).mkdir(parents=True)
            for group, names in (("windows-companion", ("KMSwitcher-Companion-0.1.0-windows-x64-setup.exe", "KMSwitcher-Companion-0.1.0-windows-x64-portable.zip")),
                                 ("linux-companion", ("app.deb", "app.rpm", "app.tar.gz")), ("console", ("KMSwitcher.html",)),
                                 ("windows-firmware-setup", ("KMSwitcher-Firmware-Setup-0.1.0-windows-x64-setup.exe", "KMSwitcher-Firmware-Setup-0.1.0-windows-x64-portable.zip")),
                                 ("linux-firmware-setup", ("setup.deb", "setup.rpm", "setup.tar.gz")),
                                 ("linux-stress", ("stress.deb", "stress.rpm", "stress.tar.gz"))):
                for name in names: (inputs / group / name).write_bytes(b"download")
            make_bundle(inputs / "firmware")
            make_release(inputs / "stress-firmware", "stress")
            identity = dict(version="0.1.0", commit="a" * 40)
            for group in ("windows-companion", "linux-companion", "windows-firmware-setup", "linux-firmware-setup", "linux-stress"):
                product = "stress" if group == "linux-stress" else "setup" if group.endswith("-setup") else "companion"
                (inputs / group / "release.json").write_text(json.dumps(dict(identity, product=product)))
            output = root / "output"
            # The pipeline passes --output build/published, while make_archive
            # returns absolute paths. Exercise that actual CLI path combination.
            script("publish-release").assemble(inputs, Path(os.path.relpath(output)), identity)
            manifest = json.loads((output / "release-manifest.json").read_text())
            self.assertIn("START-HERE.md", [item["path"] for item in manifest["files"]])
            self.assertIn("START-HERE.html", [item["path"] for item in manifest["files"]])
            page = (output / "START-HERE.html").read_text(encoding="utf-8")
            self.assertIn('href="windows-companion/KMSwitcher-Companion-0.1.0-windows-x64-setup.exe"', page)
            self.assertIn('href="windows-firmware-setup/KMSwitcher-Firmware-Setup-0.1.0-windows-x64-setup.exe"', page)
            self.assertEqual({row["product"] for row in json.loads((output / "release-index.json").read_text())["releases"]}, {"regular", "stress"})
            for item in manifest["files"]:
                self.assertEqual(hashlib.sha256((output / item["path"]).read_bytes()).hexdigest(), item["sha256"])

    def test_catalogue_is_manual_and_never_rebuilds_components(self):
        import yaml
        document = yaml.safe_load((ROOT / "azure-pipelines.yml").read_text())
        self.assertEqual(document["trigger"], "none")
        steps = document["jobs"][0]["steps"]
        self.assertTrue(any("release-catalogue.py" in step.get("bash", "") for step in steps))
        self.assertFalse(any("build-companion.py" in str(step) or "idf.ps1" in str(step) for step in steps))
