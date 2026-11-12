"""Check that release assets and the firmware index agree with their build."""
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from kms_update import releases

ROOT = Path(__file__).resolve().parents[2]
RELEASE = runpy.run_path(str(ROOT / "scripts/github-release.py"))
COMMIT = "a" * 40
REPOSITORY = "RealPlasticCheese/KMSwitcher"


class GitHubReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name, extensions in RELEASE["ARTIFACTS"].items():
            artifact = self.root / name
            artifact.mkdir()
            rows = []
            for extension in extensions.elements():
                asset = artifact / (name + extension)
                asset.write_bytes(name.encode())
                rows.append(dict(path=asset.name, sha256=RELEASE["sha256"](asset), size=asset.stat().st_size))
            document = dict(commit=COMMIT, stable=True, version="0.2.0", files=rows)
            (artifact / "component.json").write_text(json.dumps(document))
            if name in ("firmware", "stress-firmware"):
                row = dict(product="regular" if name == "firmware" else "stress", commit=COMMIT, stable=True,
                           version="0.2.0", url=rows[0]["path"], sha256=rows[0]["sha256"], size=rows[0]["size"])
                (artifact / "release-index.json").write_text(json.dumps(dict(schema=1, releases=[row])))

    def test_stage_pins_the_firmware_urls_and_hashes_all_assets(self):
        output = self.root / "output"
        RELEASE["stage"](self.root, output, COMMIT, REPOSITORY, "v0.2.0")
        index = json.loads((output / "release-index.json").read_text())
        base = f"https://github.com/{REPOSITORY}/releases/download/v0.2.0/"
        self.assertEqual([row["url"] for row in index["releases"]],
                         [base + "firmware.zip", base + "stress-firmware.zip"])
        checksums = (output / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual({line.split("  ", 1)[1] for line in checksums},
                         {path.name for path in output.iterdir() if path.name != "SHA256SUMS.txt"})

    def test_rejects_changed_asset_and_different_commit(self):
        with self.assertRaisesRegex(ValueError, "not a stable build"):
            RELEASE["collect_assets"](self.root, "b" * 40)
        (self.root / "web/web.html").write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "checksum or size"):
            RELEASE["collect_assets"](self.root, COMMIT)

    def test_public_feed_defaults_and_private_configuration_is_preserved(self):
        with patch("kms_update.firmware.default_bundle", return_value=self.root / "firmware"):
            self.assertEqual(releases.default_source(), releases.PUBLIC_INDEX)
            private = dict(azure="https://dev.azure.com/example/project", definition=10)
            (self.root / "release-source.json").write_text(json.dumps(private))
            self.assertEqual(releases.default_source(), private)


if __name__ == "__main__":
    unittest.main()
