"""Receiving policy migration and lossless, timestamped clipboard exports."""
from datetime import datetime
from pathlib import Path
import tempfile
import unittest
import json
from unittest.mock import patch

from kms_companion.clipboard_content import build_bundle, save_content, unpack_bundle
from kms_companion.clipboard_preferences import content_type, DEFAULT_RECEIVING
from kms_companion.clipboard_service import TransferService


class ReceivingTests(unittest.TestCase):
    def test_clear_removes_staged_items_and_keeps_identity_and_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            service = TransferService(directory)
            identity = service.root / "machine-id"
            identity.write_bytes(b"machine identity")
            exported = Path(directory) / "saved.txt"
            exported.write_text("saved")
            staged = service.root / "incoming" / "received"
            staged.mkdir()
            stream = (staged / "bundle.zip").open("wb")
            self.addCleanup(stream.close)
            service.incoming[(1, 2, 3)] = dict(id=7, directory=staged, stream=stream, published=True)
            service._item(7, state="Ready", prepared=True)
            (service.root / "outgoing" / "orphan").write_bytes(b"clipboard data")
            with service.send_lock, self.assertRaisesRegex(ValueError, "outgoing clipboard transfers"):
                service.clear()
            self.assertEqual(len(service.status()["items"]), 1)
            self.assertTrue(staged.exists())
            service.clear()
            service.clear()
            self.assertTrue(stream.closed)
            self.assertFalse(service.incoming)
            self.assertEqual(service.status()["items"], [])
            self.assertEqual(list((service.root / "incoming").iterdir()), [])
            self.assertEqual(list((service.root / "outgoing").iterdir()), [])
            self.assertEqual(identity.read_bytes(), b"machine identity")
            self.assertEqual(exported.read_text(), "saved")

    def test_clear_one_type_removes_only_matching_history_and_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            service = TransferService(directory)
            for identifier, kind in ((1, "text"), (2, "rich"), (3, "image"), (4, "files")):
                staged = service.root / "incoming" / f"{identifier:08x}"
                staged.mkdir()
                key = (1, 2, identifier)
                service.incoming[key] = dict(id=identifier, key=key, directory=staged, published=kind == "files")
                service._item(identifier, direction="receive", state="Ready", content_type=kind)
            service._item(5, direction="send", state="Complete", content_type="rich")
            service._item(6, direction="send", state="Complete", content_type="text")
            export = Path(directory) / "saved.html"
            export.touch()
            service._item(2, saved_path=str(export))

            service.clear("rich")

            self.assertEqual(set(service.items), {1, 3, 4, 6})
            self.assertEqual({path.name for path in (service.root / "incoming").iterdir()},
                             {"00000001", "00000003", "00000004"})
            self.assertTrue(export.exists())
            self.assertTrue(service.incoming[(1, 2, 4)]["published"])

    def test_inspect_uses_named_staging_and_falls_back_to_saved_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            service = TransferService(directory)
            staged = service.root / "incoming" / "00000001"
            bundle = staged / "bundle.zip"
            original = build_bundle(bundle, {"text/plain": b"Hello", "text/html": b"<b>Hello</b>"})
            manifest = unpack_bundle(bundle, staged / "content")
            self.assertEqual(original["formats"]["text/plain"], "formats/0")
            self.assertEqual(manifest["formats"], {"text/plain": "formats/Clipboard.txt",
                                                   "text/html": "formats/Clipboard.html"})
            self.assertEqual(json.loads((staged / "content" / "manifest.json").read_text()), manifest)
            incoming = dict(id=1, directory=staged, manifest=manifest, session=service.session)
            service.incoming[(1, 2, 3)] = incoming
            with patch.object(service, "ready", return_value=True):
                result = service.inspect(1)
                self.assertEqual(Path(result["path"]).read_bytes(), b"<b>Hello</b>")
                self.assertEqual(Path(result["folder"]), Path(result["path"]).parent)
                export = service.save(1, directory)["path"]
            self.assertNotIn("published", incoming)
            incoming["retained"] = False
            self.assertEqual(service.inspect(1), {"path": export})

    def test_existing_settings_gain_defaults_and_new_choices_persist(self):
        with tempfile.TemporaryDirectory() as directory:
            service = TransferService(directory)
            old = dict(mode="dongle", name="Laptop", max_mb=32)
            self.assertEqual(service.configure(old)["receiving"], DEFAULT_RECEIVING)
            for behaviour in ("manual", "clipboard", "save"):
                settings = dict(old, receiving={kind: behaviour for kind in DEFAULT_RECEIVING}, save_folder=directory)
                expected = service.configure(settings)
                self.assertEqual(TransferService(directory).settings, expected)
            for invalid in ({"rich": "unknown"}, {"file": "manual"}, {"text": []}, []):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    service.configure(dict(old, receiving=invalid))
                self.assertEqual(TransferService(directory).settings, expected)
            with self.assertRaisesRegex(ValueError, "folder"):
                service.configure(dict(old, receiving={"image": "save"}))

    def test_type_comes_from_formats_with_file_then_image_precedence(self):
        for formats, roots, expected in (({"text/plain": "a"}, [], "text"),
                ({"text/plain": "a", "text/html": "b"}, [], "rich"),
                ({"application/rtf": "a"}, [], "rich"),
                ({"text/html": "a", "image/png": "b"}, [], "image"),
                ({"text/plain": "a"}, ["files/test"], "files")):
            self.assertEqual(content_type(dict(kind="text", formats=formats, roots=roots)), expected)

    def test_exports_all_representations_and_never_overwrites(self):
        formats = {"text/plain": "Hello 😀".encode(), "text/html": b"<b>Hello</b>",
                   "text/rtf": b"{\\rtf1 Hello}", "application/rtf": b"duplicate RTF", "image/png": b"PNG bytes"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_bundle(root / "bundle.zip", formats)
            manifest = unpack_bundle(root / "bundle.zip", root / "staging")
            self.assertEqual((root / "staging" / manifest["formats"]["application/rtf"]).read_bytes(),
                             formats["application/rtf"])
            with patch("kms_companion.clipboard_content.datetime") as clock:
                clock.now.return_value = datetime(2026, 11, 4, 9, 8, 7, 123456)
                primary = Path(save_content(root / "staging", manifest, root))
                self.assertEqual(primary.name, "20261104-090807-123456.png")
                for mime, extension in (("text/plain", ".txt"), ("text/html", ".html"), ("text/rtf", ".rtf"), ("image/png", ".png")):
                    self.assertEqual(primary.with_suffix(extension).read_bytes(), formats[mime])
                second = Path(save_content(root / "staging", manifest, root))
                self.assertEqual(second.name, "20261104-090807-123456-1.png")
                for mime, extension in (("text/plain", ".txt"), ("text/html", ".html"), ("text/rtf", ".rtf"), ("image/png", ".png")):
                    self.assertEqual(primary.with_suffix(extension).read_bytes(), formats[mime])
                    self.assertEqual(second.with_suffix(extension).read_bytes(), formats[mime])

    def test_failed_export_removes_only_its_own_partial_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "html").write_bytes(b"<b>Hello</b>")
            manifest = dict(roots=[], formats={"text/html": "html", "text/plain": "missing"})
            with self.assertRaises(FileNotFoundError):
                save_content(root, manifest, root)
            self.assertEqual([p.name for p in root.iterdir()], ["html"])
