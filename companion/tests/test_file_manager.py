import asyncio
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, Mock, patch

from kms_companion.file_manager import _show_items, reveal_path


class FileManagerTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "Saved files, café & notes"
        self.path.mkdir()

    def test_windows_selects_saved_folder_without_a_shell(self):
        with patch("kms_companion.file_manager.sys.platform", "win32"), \
             patch("kms_companion.file_manager.subprocess.CREATE_NO_WINDOW", 0, create=True), \
             patch("kms_companion.file_manager.subprocess.Popen") as launch:
            reveal_path(self.path)
        self.assertEqual(launch.call_args.args, (f'explorer.exe /select,"{self.path.resolve()}"',))
        self.assertFalse(launch.call_args.kwargs.get("shell", False))

    def test_linux_requests_selection_and_falls_back_to_parent(self):
        with patch("kms_companion.file_manager.sys.platform", "linux"), \
             patch("kms_companion.file_manager._show_items", new_callable=AsyncMock) as select, \
             patch("kms_companion.file_manager.subprocess.run") as open_folder:
            reveal_path(self.path)
            select.assert_awaited_once_with(self.path.resolve().as_uri())
            open_folder.assert_not_called()
            select.side_effect = OSError("File manager does not support selection")
            reveal_path(self.path)
            self.assertEqual(open_folder.call_args.args[0], ["xdg-open", str(self.path.parent.resolve())])
            self.assertTrue(open_folder.call_args.kwargs["check"])

    def test_missing_saved_folder_reports_error_without_launching(self):
        with patch("kms_companion.file_manager.subprocess.Popen") as launch, \
             patch("kms_companion.file_manager.subprocess.run") as open_folder:
            with self.assertRaises(FileNotFoundError):
                reveal_path(self.path / "removed")
            launch.assert_not_called()
            open_folder.assert_not_called()

    def test_file_manager_dbus_request_and_connection_cleanup(self):
        message = Mock()
        bus = SimpleNamespace(connect=AsyncMock(), call=AsyncMock(), disconnect=Mock())
        module = SimpleNamespace(Message=message, MessageType=SimpleNamespace(ERROR="error"))
        aio = SimpleNamespace(MessageBus=Mock(return_value=bus))
        with patch.dict("sys.modules", {"dbus_next": module, "dbus_next.aio": aio}):
            bus.call.return_value = SimpleNamespace(message_type="success")
            uri = self.path.resolve().as_uri()
            asyncio.run(_show_items(uri))
            message.assert_called_once_with(destination="org.freedesktop.FileManager1",
                path="/org/freedesktop/FileManager1", interface="org.freedesktop.FileManager1",
                member="ShowItems", signature="ass", body=[[uri], ""])
            bus.disconnect.assert_called_once()
            bus.disconnect.reset_mock()
            bus.call.return_value = SimpleNamespace(message_type="error")
            with self.assertRaises(OSError):
                asyncio.run(_show_items(uri))
            bus.disconnect.assert_called_once()
