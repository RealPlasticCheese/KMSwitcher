"""Optional Qt settings/tray frontend; all commands use the shared helper."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from threading import Event
import copy
import json
import logging
from pathlib import Path
import shutil
import sys
from . import ipc
from .clipboard_preferences import CONTENT_LABELS, automatic_modes
from .power import power_label
from .protocol import default_config, encode_config, join_tolerances


def validate_document(doc):
    """Validate the entire import before changing any editor controls."""
    try:
        encode_config(doc["config"])
        revision = doc["revision"]
        if revision is not None and (type(revision) is not int or not 0 <= revision <= 0xffffffff):
            raise ValueError("Layout revision must be an unsigned integer or null for an offline draft")
    except (KeyError, TypeError, AttributeError) as error:
        raise ValueError("Expected a layout document with config and revision") from error
    return copy.deepcopy(dict(config=doc["config"], revision=revision))


def smoke_test():
    """Exercise Qt polling in the shipped interpreter without starting a helper."""
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance() or QApplication(sys.argv[:1])
    window = launch(execute=False)
    window.timer.stop()
    state = dict(settings={}, items=[], peers=[dict(slot=slot, available=False, reason="Offline")
                                               for slot in range(1, 8)])
    try:
        for index in range(3000):
            window.show_clipboard_status(state)
            if index % 100 == 0:
                app.processEvents()
    finally:
        window.pool.shutdown(wait=True, cancel_futures=True)
        window.transfer_pool.shutdown(wait=True, cancel_futures=True)
        window.device_pool.shutdown(wait=True, cancel_futures=True)
        window.tray.hide()
        window.hide()
    print("PASS: companion UI polling runtime")


def launch(*, execute=True, tray_only=False):
    from .icons import application_icon
    from .tray_menu import TrayMenu
    from .layout_view import (DESK_BACKGROUND, EDGE_STYLES, LayoutView, ScreenItem, StationItem,
                              crossing_legend, desk_label, desk_separations, edge_pens, overlap_brush,
                              shared_edges, snap_position)
    from PySide6.QtCore import QLineF, QMimeDatabase, QRegularExpression, Qt, QTimer, QUrl
    from PySide6.QtGui import (QAction, QColor, QCursor, QDesktopServices, QIcon, QPainter, QPen, QPixmap,
                               QRegularExpressionValidator)
    from PySide6.QtWidgets import (QApplication, QCheckBox, QFileDialog, QGraphicsScene,
        QGridLayout, QHBoxLayout, QLabel, QMainWindow, QMenu, QMessageBox, QPushButton,
        QSpinBox, QSystemTrayIcon, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
        QFormLayout, QComboBox, QLineEdit, QTabWidget, QScrollArea)

    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationName("KMSwitcher")
    app.setWindowIcon(application_icon())
    app.setQuitOnLastWindowClosed(False)
    instance = None
    if execute:
        from .ui_instance import UiInstance
        from .cli import ensure_helper
        instance = UiInstance()
        if not instance.acquire(b"tray" if tray_only else b"show"):
            return None
        try:
            ensure_helper()
        except Exception:
            instance.close()
            raise

    class Window(QMainWindow):
        def __init__(self):
            super().__init__()
            self.setWindowTitle("KMSwitcher — Settings")
            self.resize(1120, 820)
            self.pool = ThreadPoolExecutor(max_workers=1)
            self.transfer_pool = ThreadPoolExecutor(max_workers=1)
            self.device_pool = ThreadPoolExecutor(max_workers=1)
            self.device_stop = Event()
            self.pending = []
            self.document = None
            self.groups = {}
            self.contact_lines = []
            self.separation_items = []
            self.peers = []
            self.topology_revision = None
            self.dirty = False
            self.quitting = False
            self.updating_layout = False
            self.edit_generation = 0
            self.deferred_peers = None
            self.clipboard_state = {}
            self.clipboard_applied = set()
            self.clipboard_processing = set()
            self.clipboard_notified = {}
            self.clipboard_arrivals = set()
            self.tray_message_transfer = None
            self.clipboard_auto_timer = QTimer(self)
            self.clipboard_auto_timer.setSingleShot(True)
            self.clipboard_auto_timer.setInterval(100)
            self.clipboard_auto_timer.timeout.connect(self.send_automatic_clipboard)
            app.clipboard().dataChanged.connect(self.clipboard_changed)
            self.local_slot = 0
            self.transfer_dialog = None
            self.log_dialog = None
            self.radio_dialog = None
            self.recovery_cursor = 0
            self.recovery_session = None
            self.setWindowIcon(app.windowIcon())
            root, box = QWidget(), QVBoxLayout()
            root.setLayout(box)
            self.setCentralWidget(root)
            self.status_label = QLabel("Connecting to local helper…")
            self.status_label.setWordWrap(True)
            box.addWidget(self.status_label)
            self.tabs = QTabWidget()
            box.addWidget(self.tabs, 1)
            general = QWidget(); general_box = QVBoxLayout(general)
            self.tabs.addTab(general, "General")
            actions = QHBoxLayout()
            firmware = QPushButton("Open Firmware Setup…")
            firmware.clicked.connect(self.open_firmware); actions.addWidget(firmware)
            restart = QPushButton("Restart dongles…")
            restart.clicked.connect(self.restart_dongles); actions.addWidget(restart)
            logs = QPushButton('Local dongle logs…')
            logs.clicked.connect(self.show_diagnostics); actions.addWidget(logs)
            about = QPushButton("About")
            about.clicked.connect(self.about); actions.addWidget(about); actions.addStretch()
            general_box.addLayout(actions)
            tools_row = QHBoxLayout()
            for label, action in (("Locate Firmware Setup…", lambda: self.open_firmware(configure=True)),
                                  ("Open Engineering Console…", self.open_engineering),
                                  ("Locate Engineering Console…", lambda: self.open_engineering(configure=True))):
                control = QPushButton(label); control.clicked.connect(action); tools_row.addWidget(control)
            tools_row.addStretch(); general_box.addLayout(tools_row)
            from .cli import startup_enabled
            self.start_at_login = QCheckBox("Start the companion when I sign in")
            self.start_at_login.setChecked(startup_enabled())
            self.start_at_login.toggled.connect(self.configure_startup)
            general_box.addWidget(self.start_at_login)
            self.recovery_notifications = QCheckBox('Show automatic recovery notifications')
            self.recovery_notifications.setChecked(True)
            self.recovery_notifications.clicked.connect(lambda enabled: self.submit('recovery-notifications', enabled=enabled))
            general_box.addWidget(self.recovery_notifications)
            parking = QHBoxLayout()
            parking.addWidget(QLabel("Park pointer after an edge switch:"))
            self.pointer_parking = QComboBox()
            from .geometry import POINTER_PARKING_MODES
            for mode in POINTER_PARKING_MODES:
                self.pointer_parking.addItem(mode.replace("-", " ").capitalize(), mode)
            self.pointer_parking.setToolTip("Applies to this computer after a confirmed mouse edge switch. Uses the outermost screen edge; centre positions use the nearest real screen point if there is a gap. Keyboard and manual switches leave the pointer in place.")
            self.pointer_parking.activated.connect(lambda index: self.submit(
                "pointer-parking", mode=self.pointer_parking.itemData(index)))
            parking.addWidget(self.pointer_parking)
            parking.addStretch()
            general_box.addLayout(parking)
            general_box.addStretch()
            desk = QWidget(); desk_box = QVBoxLayout(desk)
            self.tabs.addTab(desk, "Desk layout")
            controls = QHBoxLayout()
            self.local_enabled = QCheckBox("Enable automatic switching on this computer")
            self.local_enabled.clicked.connect(lambda enabled: self.submit("auto", enabled=enabled))
            controls.addWidget(self.local_enabled)
            self.global_enabled = QCheckBox("Enable across the desk")
            self.global_enabled.clicked.connect(self.mark_dirty)
            controls.addWidget(self.global_enabled)
            controls.addStretch()
            desk_box.addLayout(controls)
            instructions = QLabel("Drag a dongle's screens to arrange your desk. Nearby edges snap together. Offline dongles keep their remembered screens.")
            instructions.setWordWrap(True); desk_box.addWidget(instructions)
            self.scene = QGraphicsScene()
            self.scene.setBackgroundBrush(QColor(DESK_BACKGROUND))
            self.view = LayoutView(self.scene)
            self.view.setRenderHint(QPainter.RenderHint.Antialiasing)
            self.view.setMinimumHeight(260)
            desk_box.addWidget(self.view, 1)
            self.crossing_key = crossing_legend(desk)
            desk_box.addWidget(self.crossing_key)
            self.crossing_status = QLabel()
            self.crossing_status.setWordWrap(True)
            desk_box.addWidget(self.crossing_status)
            self.table = QTableWidget(7, 5)
            self.table.setHorizontalHeaderLabels(["Dongle", "In layout", "Screens", "Availability", "PC power"])
            self.table.setMaximumHeight(270)
            self.table.horizontalHeader().setStretchLastSection(True)
            for i in range(7):
                slot = QTableWidgetItem(str(i + 1)); slot.setFlags(slot.flags() & ~Qt.ItemFlag.ItemIsEditable)
                self.table.setItem(i, 0, slot)
                check = QCheckBox()
                check.clicked.connect(lambda checked, row=i: self.toggle_station(row, checked))
                self.table.setCellWidget(i, 1, check)
                for col, text in ((2, "Not reported"), (3, "Unknown"), (4, "Unknown")):
                    item = QTableWidgetItem(text); item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEditable)
                    self.table.setItem(i, col, item)
            desk_box.addWidget(self.table)
            self.table.currentCellChanged.connect(self.select_station)
            tolerance_options = QHBoxLayout()
            self.gap_tolerance = QLineEdit()
            self.overlap_tolerance = QLineEdit()
            for name, field in (("Gap tolerance (px)", self.gap_tolerance), ("Overlap tolerance (px)", self.overlap_tolerance)):
                field.setAccessibleName(name)
                field.setValidator(QRegularExpressionValidator(QRegularExpression("[0-9]+"), field))
                field.setMaximumWidth(110)
                field.textEdited.connect(self.mark_dirty)
                label = QLabel(name)
                label.setBuddy(field)
                tolerance_options.addWidget(label)
                tolerance_options.addWidget(field)
            tolerance_options.addStretch()
            desk_box.addLayout(tolerance_options)
            options = QHBoxLayout()
            self.delay, self.corner = QSpinBox(), QSpinBox()
            self.delay.setRange(0, 2000); self.delay.setSuffix(" ms")
            self.corner.setRange(0, 64); self.corner.setSuffix(" px")
            self.delay.setToolTip("How long to keep pushing against a screen edge before switching computers. 0 ms switches without an added wait.")
            self.corner.setToolTip("Do not switch within this many pixels of a screen corner. 0 px allows switching at corners too.")
            self.delay.valueChanged.connect(self.mark_dirty); self.corner.valueChanged.connect(self.mark_dirty)
            options.addWidget(QLabel("Edge pressure:")); options.addWidget(self.delay)
            options.addWidget(QLabel("Corner exclusion:")); options.addWidget(self.corner)
            options.addStretch()
            for text, action in (("Reload", self.reload), ("Import…", self.import_config),
                                 ("Export…", self.export_config), ("Fit layout", self.view.fit_layout),
                                 ("Save to master", self.save)):
                button = QPushButton(text); button.clicked.connect(action); options.addWidget(button)
            desk_box.addLayout(options)
            from .settings_ui import ClipboardSettings
            self.clipboard_panel = ClipboardSettings(self.submit)
            self.add_settings_tab(self.clipboard_panel, "Clipboard")
            self.create_transfers_tab()
            self.device_panel = QWidget(); self.device_box = QVBoxLayout(self.device_panel)
            self.device_note = QLabel("Connect a receiver, then load device preferences.")
            self.device_note.setWordWrap(True); self.device_box.addWidget(self.device_note)
            load_device = QPushButton("Reload device preferences")
            load_device.clicked.connect(self.load_preferences); self.device_box.addWidget(load_device)
            self.device_form = None
            self.device_box.addStretch()
            from .device_management_ui import DeviceManagement
            self.device_management = DeviceManagement(self.device_panel, self.device_task)
            self.add_settings_tab(self.device_management, "Devices")
            from .radio_channel_ui import create_dialog
            self.radio_dialog = create_dialog(self, self.submit)
            self.radio_dialog.setWindowFlags(Qt.WindowType.Widget)
            self.add_settings_tab(self.radio_dialog, "Radio")
            from .diagnostics_ui import create_dialog
            from .remote_diagnostics_ui import RemoteDiagnosticsPanel
            self.diagnostics_tabs = QTabWidget()
            self.log_dialog = create_dialog(self, self.refresh_diagnostics)
            self.log_dialog.setWindowFlags(Qt.WindowType.Widget)
            self.diagnostics_tabs.addTab(self.log_dialog, 'Local dongle')
            self.diagnostics_tabs.addTab(RemoteDiagnosticsPanel(self.device_task), 'Remote dongles')
            self.tabs.addTab(self.diagnostics_tabs, 'Diagnostics')
            from .shortcuts_ui import ShortcutsPanel
            self.shortcuts_panel = ShortcutsPanel()
            self.tabs.addTab(self.shortcuts_panel, "Shortcuts")
            self.tabs.currentChanged.connect(self.settings_tab_changed)
            self.tabs.setCurrentIndex(1)
            self.note = QLabel("Placement and remembered screens belong to the dongle. Screen changes are reported by the attached computer. Manual hotkeys remain available.")
            self.note.setWordWrap(True); box.addWidget(self.note)
            self.tray = QSystemTrayIcon(self.windowIcon(), self)
            self.tray_menu = TrayMenu(self)
            menu = self.tray_menu
            self.send_actions = {}
            for slot in range(1, 8):
                action = menu.addAction(f"Send clipboard to Dongle {slot}")
                action.setEnabled(False)
                action.setVisible(False)
                self.transfer_send_menu.addAction(action)
                action.triggered.connect(lambda checked=False, target=slot: self.send_clipboard(target))
                self.send_actions[slot] = action
            menu.addSeparator()
            self.incoming_action = menu.addAction("Clipboard transfers…", self.show_transfers)
            menu.addSeparator()
            menu.aboutToShow.connect(lambda: self.submit("clipboard-status", callback=self.show_clipboard_status, ui=True))
            self.tray_enabled = QAction("Enable automatic switching", self)
            self.tray_enabled.setCheckable(True)
            self.tray_enabled.triggered.connect(lambda enabled: self.submit("auto", enabled=enabled))
            menu.addAction(self.tray_enabled)
            menu.addAction("Settings", self.open_settings)
            menu.addAction("Open Firmware Setup", self.open_firmware)
            menu.addAction("Restart dongles", self.restart_dongles)
            menu.addAction("Quit companion", self.quit_companion)
            if sys.platform != "win32":
                self.tray.setContextMenu(menu)
            self.tray.activated.connect(self.tray_activated)
            self.tray.messageClicked.connect(self.tray_message_clicked)
            self.tray.show()
            self.timer = QTimer(self)
            self.timer.timeout.connect(self.tick); self.timer.start(100)
            self.status_ticks = 0
            self.loaded(dict(config=default_config(), revision=None))
            self.note.setText("Offline draft: arrange and export a layout, or connect a receiver and Reload to edit the master.")
            self.submit("status", callback=self.show_status)
            self.submit("clipboard-status", callback=self.show_clipboard_status, ui=True)
            self.reload()

        def submit(self, command, callback=None, **args):
            pool = self.transfer_pool if command in ("clipboard-save", "clipboard-content", "clipboard-dismiss",
                                                     "clipboard-clear", "clipboard-inspect") else self.pool
            future = pool.submit(ipc.call, command, **args)
            future.kms_command = command
            self.pending.append((future, callback))
            return future

        def add_settings_tab(self, panel, title):
            scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setFrameShape(QScrollArea.Shape.NoFrame)
            scroll.setWidget(panel)
            self.tabs.addTab(scroll, title)

        def device_task(self, work, done, failed):
            def call(command, **args):
                if self.device_stop.is_set(): raise RuntimeError('Device operation cancelled')
                return ipc.call(command, **args)
            future = self.device_pool.submit(work, call)
            future.kms_command = 'device-management'
            future.kms_error = failed
            self.pending.append((future, done))

        def settings_tab_changed(self, index):
            title = self.tabs.tabText(index)
            if title == "Devices" and self.device_form is None and not any(f.kms_command == "preferences-get" for f, _ in self.pending):
                self.load_preferences()
            elif title == "Radio" and self.radio_dialog.state is None:
                self.radio_dialog.load()
            elif title == "Shortcuts":
                future = self.submit("preferences-get", callback=self.shortcuts_panel.show_preferences)
                future.kms_error = lambda error: self.shortcuts_panel.show_unavailable()

        def select_settings_tab(self, title):
            for index in range(self.tabs.count()):
                if self.tabs.tabText(index) == title:
                    self.tabs.setCurrentIndex(index)
                    break
            self.open_settings()

        def restart_dongles(self):
            from kms_transport.tools import open_setup
            open_setup(self, 'restart')

        def open_firmware(self, *, configure=False):
            from kms_transport.tools import open_setup
            open_setup(self, configure=configure)

        def open_engineering(self, *, configure=False):
            from kms_transport.tools import open_console
            open_console(self, configure=configure)

        def show_radio_channel(self):
            self.select_settings_tab("Radio")

        def show_diagnostics(self):
            self.diagnostics_tabs.setCurrentWidget(self.log_dialog)
            self.select_settings_tab('Diagnostics')
            self.refresh_diagnostics()

        def refresh_diagnostics(self):
            self.submit('diagnostics-refresh', callback=self.log_dialog.show_state)

        def configure_startup(self, enabled):
            from .cli import startup
            try:
                startup(enabled)
            except OSError as error:
                self.start_at_login.blockSignals(True)
                self.start_at_login.setChecked(not enabled)
                self.start_at_login.blockSignals(False)
                QMessageBox.warning(self, "Login startup", str(error))

        def about(self):
            from . import __version__, BUILD_INFO
            QMessageBox.about(self, "About KMSwitcher", f"KMSwitcher {__version__}<br>Commit: {BUILD_INFO.get('commit', 'source checkout')}<br>"
                              'By <a href="https://plasticcheese.com/">Frank Tzanabetis</a><br>'
                              "Third-party notices are in the installation folder.")

        def tick(self):
            for future, callback in self.pending[:]:
                if not future.done():
                    continue
                self.pending.remove((future, callback))
                identifier = getattr(future, "clipboard_id", None)
                if identifier is not None:
                    self.clipboard_processing.discard(identifier)
                try:
                    result = future.result()
                    if callback:
                        callback(result)
                except Exception as error:
                    if future.exception() is None:
                        logging.getLogger(__name__).exception('UI callback failed for %s', future.kms_command)
                    self.note.setText(str(error))
                    if hasattr(future, 'kms_error'):
                        future.kms_error(error)
                        continue
                    if future.kms_command.startswith('channel-') and self.radio_dialog:
                        self.radio_dialog.show_error(error, future.kms_command)
                    if future.kms_command.startswith('diagnostics-') and self.log_dialog:
                        self.log_dialog.show_state(dict(loading=False, entries=[], error=str(error)))
                    if future.kms_command == "config-set":
                        self.report_error("Layout not saved", error)
                    if future.kms_command in ("preferences-get", "preferences-set", "preferences-default"):
                        self.device_note.setText(str(error))
                    if future.kms_command == "clipboard-settings":
                        self.clipboard_panel.message.setText(str(error))
                    capture = getattr(future, "clipboard_capture", None)
                    if capture:
                        shutil.rmtree(Path(capture).parent, ignore_errors=True)
                    # Background polling must not flood the desktop while the helper is unavailable.
                    if future.kms_command.startswith("clipboard-") and future.kms_command not in (
                            "clipboard-status", "clipboard-auto-send"):
                        self.show_tray_message("Clipboard", str(error))
            self.status_ticks += 1
            polling = any(future.kms_command in ("status", "clipboard-status") for future, _ in self.pending)
            if self.status_ticks >= 10 and not polling:
                self.status_ticks = 0
                self.submit("status", callback=self.show_status)
                self.submit("clipboard-status", callback=self.show_clipboard_status, ui=True)
                if self.log_dialog and self.log_dialog.isVisible() and not any(
                        f.kms_command.startswith('diagnostics-') for f, _ in self.pending):
                    self.submit('diagnostics-status', callback=self.log_dialog.show_state)
            elif self.status_ticks % 2 == 0 and not polling:
                self.submit("clipboard-status", callback=self.show_clipboard_status, ui=True)

        def show_status(self, s):
            self.recovery_notifications.setChecked(s.get('recovery_notifications', True))
            self.pointer_parking.setCurrentIndex(max(0, self.pointer_parking.findData(s.get("pointer_parking", "bottom-right"))))
            events = s.get('recovery_events', [])
            session = s.get('recovery_session')
            if session != self.recovery_session:
                self.recovery_session = session
                self.recovery_cursor = max((e['id'] for e in events), default=0)
            for event in events:
                if event['id'] <= self.recovery_cursor: continue
                self.recovery_cursor = event['id']
                if event.get('notify') and self.recovery_notifications.isChecked():
                    self.show_tray_message('KMSwitcher recovery', event['message'])
            self.local_slot = s.get("slot", 0)
            self.local_enabled.setChecked(s.get("enabled", False))
            self.tray_enabled.setChecked(s.get("enabled", False))
            text = f"Dongle {s.get('slot', '?')} · selected {s.get('selected', '?')} · {s.get('backend', 'Waiting')} · {s.get('reason', '')}"
            self.status_label.setText(text); self.tray.setToolTip(text)
            topology = s.get("topology_revision")
            connected = s.get("connected", s.get("revision") is not None)
            if not connected:
                self.topology_revision = None
                if self.peers:
                    self.loaded_peers([dict(p, eligible=False, ready=False, session=0, age_ms=3000) for p in self.peers])
                return
            if not s.get('master_connected', True): return
            if self.document["revision"] is None and not self.dirty and not any(
                    future.kms_command == "config-get" for future, _ in self.pending):
                self.request_reload()
            if topology is not None and topology != self.topology_revision and not any(
                    future.kms_command == "peers" for future, _ in self.pending):
                self.submit("peers", callback=lambda peers: self.loaded_peers(peers, topology), include_joins=True)
            if self.document["revision"] is not None and s.get("revision") != self.document["revision"]:
                self.note.setText("Master configuration changed. Reload before saving; your edits have been retained.")

        def send_clipboard(self, slot):
            if not self.transfer_clear_button.isEnabled():
                return
            from .agent import config_directory
            from .clipboard_ui import capture_clipboard
            try:
                limit = self.clipboard_state.get("settings", {}).get("max_mb", 256) * 1024 * 1024
                capture = capture_clipboard(app.clipboard(), config_directory(), limit)
                self.submit("clipboard-send", slot=slot, capture=capture)
            except (OSError, ValueError) as error:
                self.show_tray_message("Clipboard not sent", str(error))
                self.note.setText(str(error))

        def clipboard_changed(self):
            settings = self.clipboard_state.get("settings", {})
            if not self.quitting and any(mode != "off" for mode in automatic_modes(settings).values()):
                self.clipboard_auto_timer.start()

        def send_automatic_clipboard(self):
            from .agent import config_directory
            from .clipboard_ui import capture_clipboard
            settings = self.clipboard_state.get("settings", {})
            session = self.clipboard_state.get("ready_session")
            enabled_types = {kind for kind, mode in automatic_modes(settings).items() if mode != "off"}
            if self.quitting or not session or not enabled_types:
                return
            if not self.transfer_clear_button.isEnabled() or any(
                    future.kms_command == "clipboard-auto-send" for future, _ in self.pending):
                self.clipboard_auto_timer.start()
                return
            try:
                capture = capture_clipboard(app.clipboard(), config_directory(),
                                            settings["max_mb"] * 1024 * 1024, automatic_types=enabled_types)
                if capture:
                    future = self.submit("clipboard-auto-send", capture=capture, source_session=session)
                    future.clipboard_capture = capture
            except (OSError, ValueError) as error:
                self.note.setText(str(error))

        def notify_clipboard_arrival(self, identifier, title, detail):
            if identifier in self.clipboard_arrivals:
                return
            self.clipboard_arrivals.add(identifier)
            item = next((item for item in self.clipboard_state.get("items", []) if item["id"] == identifier), {})
            kind = item.get("content_type", item.get("kind"))
            if self.clipboard_state.get("settings", {}).get("arrival_popups", {}).get(kind, True):
                self.show_tray_message(title, detail, transfer_id=identifier)

        def show_tray_message(self, title, detail, *, transfer_id=None):
            # Qt's click signal has no payload, so keep the target of the latest message.
            self.tray_message_transfer = transfer_id
            self.tray.showMessage(title, detail)

        def tray_message_clicked(self):
            identifier = self.tray_message_transfer
            self.tray_message_transfer = None
            if identifier is not None:
                self.show_transfers(transfer_id=identifier)

        def show_clipboard_status(self, state):
            from .clipboard_preferences import DEFAULT_RECEIVING
            self.clipboard_state = state
            settings = state.get("settings", {})
            if settings.get("name") and not self.clipboard_panel.loaded and not self.clipboard_panel.dirty:
                self.clipboard_panel.load(settings)
            peers = {p["slot"]: p for p in state.get("peers", [])}
            configured = {s["slot"] for s in (self.document or {}).get("config", {}).get("stations", []) if s["configured"]}
            self.clipboard_panel.show_peers(peers.values(), configured - {self.local_slot})
            for slot, action in self.send_actions.items():
                peer = peers.get(slot, {})
                name = " — " + peer["name"] if peer.get("name") else ""
                action.setText(f"Send clipboard to Dongle {slot}{name}")
                action.setVisible(slot != self.local_slot and (slot in peers or slot in configured))
                action.setEnabled(bool(peer.get("available")))
                action.setToolTip(peer.get("reason", "No clipboard-capable companion; update both firmwares if needed"))
            self.transfer_send_button.setEnabled(self.transfer_clear_button.isEnabled() and
                                                 any(action.isVisible() for action in self.send_actions.values()))
            items = state.get("items", [])
            current_ids = {item["id"] for item in items}
            self.clipboard_applied.intersection_update(current_ids)
            self.clipboard_arrivals.intersection_update(current_ids)
            self.incoming_action.setText(f"Clipboard transfers… ({len(items)})")
            for item in items:
                identifier = item["id"]
                kind = item.get("content_type", item.get("kind"))
                behaviour = settings.get("receiving", {}).get(kind, DEFAULT_RECEIVING.get(kind, "manual"))
                automatic = (item.get("direction") == "receive" and item.get("state") == "Ready" and
                             identifier not in self.clipboard_applied and identifier not in self.clipboard_processing)
                if automatic and behaviour == "clipboard":
                    self.accept_clipboard(identifier)
                elif automatic and behaviour == "save":
                    self.clipboard_applied.add(identifier)
                    self.save_received_files(identifier, parent=settings.get("save_folder"), automatic=True)
                previous = self.clipboard_notified.get(identifier)
                current = item.get("state")
                title = f"Clipboard: {current} · Dongle {item.get('slot', '?')}"
                if item.get("direction") == "receive" and current == "Ready" and behaviour == "manual":
                    self.notify_clipboard_arrival(identifier, title, "Open Transfers to accept or save the received content")
                elif current != previous and current in ("Complete", "Failed", "Cancelled") and not item.get("automatic"):
                    self.show_tray_message(title, item.get("detail", current))
            self.clipboard_notified = {item["id"]: item["state"] for item in items}
            self.refresh_transfers()

        def accept_clipboard(self, identifier):
            if identifier in self.clipboard_processing:
                return
            from .clipboard_ui import publish_clipboard
            self.clipboard_applied.add(identifier)
            self.clipboard_processing.add(identifier)
            def apply(content):
                publish_clipboard(app.clipboard(), content)
                self.submit("clipboard-applied", id=identifier)
                self.note.setText("Received content is ready for normal Paste")
                self.notify_clipboard_arrival(identifier, "Clipboard received", "Received content is ready to paste")
            future = self.submit("clipboard-content", id=identifier, callback=apply)
            if future is not None:
                future.clipboard_id = identifier

        def clipboard_preferences(self):
            self.select_settings_tab("Clipboard")

        def create_transfers_tab(self):
            if self.transfer_dialog is None:
                self.transfer_dialog = QWidget()
                layout = QVBoxLayout(self.transfer_dialog)
                label = QLabel("Receiving behaviour is set on the Clipboard tab. Accept or save individual items here. Prepared files remain available until the companion restarts.")
                label.setWordWrap(True); layout.addWidget(label)
                folder_link = QLabel('<a href="clipboard-folder">Open transfer folder</a>')
                folder_link.linkActivated.connect(self.open_transfer_folder)
                layout.addWidget(folder_link)
                self.transfer_send_button = QPushButton("Send clipboard")
                self.transfer_send_button.setEnabled(False)
                self.transfer_send_menu = QMenu(self.transfer_send_button)
                self.transfer_send_menu.setToolTipsVisible(True)
                self.transfer_send_button.setMenu(self.transfer_send_menu)
                self.transfer_clear_button = QPushButton("Clear All")
                self.transfer_clear_button.clicked.connect(lambda: self.clear_transfers())
                controls = QHBoxLayout()
                controls.addWidget(self.transfer_send_button)
                controls.addWidget(self.transfer_clear_button)
                self.transfer_clear_by_type = {}
                for kind, label in (("text", "Plain text"), ("rich", "Plain Formatted text"),
                                    ("image", "Images"), ("files", "Files and Folders")):
                    button = QPushButton(f"Clear {label}")
                    button.clicked.connect(lambda checked=False, kind=kind: self.clear_transfers(kind))
                    self.transfer_clear_by_type[kind] = button
                    controls.addWidget(button)
                controls.addStretch()
                layout.addLayout(controls)
                self.transfer_speed = QLabel(); self.transfer_speed.setWordWrap(True)
                layout.addWidget(self.transfer_speed)
                self.transfer_table = QTableWidget(0, 7)
                self.transfer_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
                self.transfer_table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
                self.transfer_table.setHorizontalHeaderLabels(["ID", "Direction / computer", "Date / time", "Content", "Progress", "Status", "Actions"])
                self.transfer_table.horizontalHeader().setStretchLastSection(True)
                for column, width in enumerate((86, 170, 105, 110, 190, 100)):
                    self.transfer_table.setColumnWidth(column, width)
                self.transfer_table.verticalHeader().setDefaultSectionSize(52)
                layout.addWidget(self.transfer_table)
                self.tabs.addTab(self.transfer_dialog, "Transfers")

        def show_transfers(self, *, transfer_id=None):
            self.refresh_transfers()
            self.select_settings_tab("Transfers")
            if transfer_id is not None:
                self.select_transfer(transfer_id)
                self.transfer_table.setFocus()

        def select_transfer(self, identifier):
            table = self.transfer_table
            for row in range(table.rowCount()):
                cell = table.item(row, 0)
                if cell.data(Qt.ItemDataRole.UserRole) == identifier:
                    table.selectRow(row)
                    table.scrollToItem(cell)
                    return
            table.clearSelection()

        def open_transfer_folder(self, _link=None):
            from .agent import config_directory
            folder = config_directory() / "clipboard"
            try:
                folder.mkdir(parents=True, exist_ok=True)
                if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(folder))):
                    raise OSError("The file manager could not open the transfer folder")
            except OSError as error:
                QMessageBox.warning(self.transfer_dialog, "Open transfer folder", str(error))

        def refresh_transfers(self):
            if self.transfer_dialog is None:
                return
            stream = self.clipboard_state.get("stream", {})
            requested = self.clipboard_state.get("settings", {}).get("speed", "conservative")
            effective = ", ".join(sorted(set(stream.get("links", {}).values()))) or "waiting for a transfer"
            self.transfer_speed.setText(f"Dongle profile: {requested}; local radio: {effective}")
            items = self.clipboard_state.get("items", [])[::-1]
            table = self.transfer_table
            selected_row = table.currentRow()
            selected_cell = table.item(selected_row, 0)
            selected_id = (selected_cell.data(Qt.ItemDataRole.UserRole)
                           if selected_cell is not None and selected_cell.isSelected() else None)
            table.setRowCount(len(items))
            for row, item in enumerate(items):
                identifier = item["id"]
                created_at = item.get("created_at")
                date_time = (datetime.fromtimestamp(created_at).strftime("%x\n%X")
                             if created_at is not None else "")
                values = [f"{identifier:08x}", f"{item.get('direction', '')} · Dongle {item.get('slot', '?')}\n{item.get('name', '')}",
                          date_time,
                          {"text": "Plain text", "rich": "Formatted text", "image": "Image", "content": "Image / rich text", "files": "Files / folders"}.get(item.get("content_type", item.get("kind")), "Preparing"),
                          f"{item.get('progress', 0):,} / {item.get('size', 0):,} bytes\n{item.get('bytes_per_second', 0) / 1024:.1f} KiB/s",
                          f"{item.get('state', '')}\n{item.get('transport', '')}"]
                for column, value in enumerate(values):
                    cell = table.item(row, column)
                    if cell is None:
                        cell = QTableWidgetItem()
                        table.setItem(row, column, cell)
                    cell.setText(value); cell.setToolTip(item.get("detail", ""))
                table.item(row, 0).setData(Qt.ItemDataRole.UserRole, identifier)
                available = item.get("available", item.get("direction") == "receive" and
                                     item.get("state") in ("Ready", "On clipboard", "Saved"))
                cancellable = item.get("cancellable", item.get("state") in ("Preparing", "Sending", "Receiving"))
                saved_path = item.get("saved_path")
                action_key = (identifier, available, item.get("kind") if available else None, cancellable,
                              bool(item.get("prepared")), saved_path)
                actions = table.cellWidget(row, 6)
                if actions is not None and actions.kms_actions == action_key:
                    continue  # Keep a pressed button alive across progress polling.
                actions = QWidget()
                box = QGridLayout(actions)
                box.setContentsMargins(2, 2, 2, 2)
                box.setSpacing(2)
                actions.kms_actions = action_key
                def button(text, callback):
                    control = QPushButton(text)
                    control.clicked.connect(callback)
                    row_index, column_index = divmod(box.count(), 2)
                    box.addWidget(control, row_index, column_index)
                if available:
                    button("Prepare for paste" if item.get("kind") == "files" else "Put on clipboard",
                           lambda checked=False, i=identifier: self.accept_clipboard(i))
                    button("Save to…", lambda checked=False, i=identifier: self.save_received_files(i))
                if available or saved_path:
                    button("View", lambda checked=False, i=identifier: self.open_transfer_item(i, "path", "View"))
                if available:
                    button("Open folder", lambda checked=False, i=identifier: self.open_transfer_item(i, "folder", "Open folder"))
                if saved_path:
                    button("Open location", lambda checked=False, path=saved_path: self.open_saved_location(path))
                if cancellable:
                    button("Cancel", lambda checked=False, i=identifier: self.submit("clipboard-cancel", id=i))
                else:
                    button("Remove staged files…" if item.get("prepared") else "Dismiss",
                           lambda checked=False, i=identifier, prepared=item.get("prepared", False): self.dismiss_transfer(i, prepared))
                table.setCellWidget(row, 6, actions)
                table.setRowHeight(row, max(table.verticalHeader().defaultSectionSize(), actions.minimumSizeHint().height() + 1))
            if selected_id is not None and (selected_row >= len(items) or items[selected_row]["id"] != selected_id):
                self.select_transfer(selected_id)

        def dismiss_transfer(self, identifier, prepared):
            if prepared and QMessageBox.question(self, "Remove staged files", "These files will no longer be available to paste. Remove the companion's staged copy?") != QMessageBox.StandardButton.Yes:
                return
            self.submit("clipboard-dismiss", id=identifier, release=prepared)

        def open_transfer_item(self, identifier, key, title):
            def open_item(result):
                path = result.get(key)
                if key == "path" and result.get("file_transfer"):
                    mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase.MatchMode.MatchExtension).name()
                    # The desktop's default action can run programs and shortcuts.
                    if not (mime.startswith(("image/", "audio/", "video/")) or mime in (
                            "text/plain", "text/html", "text/rtf", "application/rtf", "application/pdf")):
                        path = result["folder"]
                if not path or not Path(path).exists():
                    raise FileNotFoundError("This clipboard item's content is no longer available")
                if not QDesktopServices.openUrl(QUrl.fromLocalFile(path)):
                    raise OSError("The default application could not open the clipboard item")

            future = self.submit("clipboard-inspect", id=identifier, callback=open_item)
            future.kms_error = lambda error: QMessageBox.warning(self, title, str(error))

        def clear_transfers(self, kind=None):
            selection = f"clipboard items of type {CONTENT_LABELS[kind]}" if kind else "all clipboard items"
            if QMessageBox.question(
                    self, "Clear clipboard transfers",
                    f"This will delete {selection} in the clipboard folder. Clear these transfers?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                    QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
                return

            buttons = (self.transfer_clear_button, *self.transfer_clear_by_type.values())
            for button in buttons:
                button.setEnabled(False)
            self.transfer_send_button.setEnabled(False)

            def finished(_result):
                for button in buttons:
                    button.setEnabled(True)
                self.submit("clipboard-status", callback=self.show_clipboard_status, ui=True)

            def failed(error):
                finished(None)
                QMessageBox.warning(self, "Clear clipboard transfers", str(error))

            future = self.submit("clipboard-clear", callback=finished, **({"kind": kind} if kind else {}))
            future.kms_error = failed

        def save_received_files(self, identifier, parent=None, automatic=False):
            if identifier in self.clipboard_processing:
                return
            if parent is None and not automatic:
                parent = QFileDialog.getExistingDirectory(self, "Save received clipboard content")
            if parent:
                self.clipboard_processing.add(identifier)
                def saved(result):
                    path = result["path"]
                    self.note.setText("Saved to " + path)
                    for item in self.clipboard_state.get("items", []):
                        if item["id"] == identifier:
                            item["saved_path"] = path
                    self.refresh_transfers()
                    if not automatic:
                        self.open_saved_location(path)
                    else:
                        self.notify_clipboard_arrival(identifier, "Clipboard saved", "Saved to " + path)
                future = self.submit("clipboard-save", id=identifier, parent=parent,
                            callback=saved, **({"automatic": True} if automatic else {}))
                if future is not None:
                    future.clipboard_id = identifier

        def open_saved_location(self, path):
            from .file_manager import reveal_path
            future = self.transfer_pool.submit(reveal_path, path)
            future.kms_command = "clipboard-open-location"
            self.pending.append((future, None))

        def reload(self):
            if self.dirty and not self.confirm_discard():
                return
            self.request_reload()

        def request_reload(self):
            generation = self.edit_generation
            def apply(doc):
                if generation == self.edit_generation:
                    self.loaded(doc)
                else:
                    self.note.setText("Layout changed while loading; edits retained. Reload when ready.")
            self.submit("config-get", callback=apply)
            self.submit("peers", callback=self.loaded_peers, include_joins=True)

        def loaded(self, doc):
            doc = validate_document(doc)
            self.document = doc
            c = doc["config"]
            self.stations = {s["slot"] - 1: copy.deepcopy(s) for s in c["stations"]}
            self.updating_layout = True
            self.global_enabled.setChecked(c["enabled"])
            self.delay.setValue(c["delay_ms"]); self.corner.setValue(c["corner"])
            gap, overlap = join_tolerances(c)
            self.gap_tolerance.setText(str(gap))
            self.overlap_tolerance.setText(str(overlap))
            for s in c["stations"]:
                i = s["slot"] - 1
                self.table.cellWidget(i, 1).setChecked(s["configured"])
            self.updating_layout = False
            self.edit_generation += 1
            self.dirty = False
            self.layout_moved = False
            self.draw_layout()
            self.note.setText("Configuration loaded. Stations without placement are excluded from automatic switching.")

        def loaded_peers(self, peers, revision=None):
            if self.scene.mouseGrabberItem():
                self.deferred_peers = (peers, revision)
                return
            if revision is not None:
                self.topology_revision = revision
            self.peers = peers
            for i in range(7):
                self.table.item(i, 2).setText("Not reported")
                self.table.item(i, 3).setText("Unknown")
            for p in peers:
                reason = "Ready" if p["eligible"] else ("Helper absent" if p["age_ms"] >= 3000 or not p["session"] else
                    "Disabled" if not p["enabled"] else "Desktop unavailable" if not p["ready"] else "Placement required")
                self.table.item(p["slot"] - 1, 2).setText(str(len(p["screens"])) if p.get("screens") else "Not reported")
                self.table.item(p["slot"] - 1, 3).setText(reason)
                self.table.item(p["slot"] - 1, 4).setText(power_label(p.get("power_state", "unknown"), p.get("power_fresh", False)))
                self.table.item(p["slot"] - 1, 4).setToolTip(
                    "Companion report; a requested transition does not prove the PC powered off."
                    if p.get("power_supported") else "Update the S3 and S2 firmware, and run the companion on this computer.")
            self.draw_layout()

        def station_screens(self, i):
            peer = next((p for p in self.peers if p["slot"] == i + 1), {})
            screens = peer.get("screens") or [dict(x=0, y=0, width=800, height=450)]
            left, top = min(r["x"] for r in screens), min(r["y"] for r in screens)
            width = max(r["x"] + r["width"] for r in screens) - left
            height = max(r["y"] + r["height"] for r in screens) - top
            area_width = self.stations[i].get("width", 0) or width
            area_height = self.stations[i].get("height", 0) or height
            scaled = []
            for r in screens:
                # Use the controller's integer boundary mapping so snaps also touch in routing coordinates.
                x = (r["x"] - left) * area_width // width
                y = (r["y"] - top) * area_height // height
                right = (r["x"] + r["width"] - left) * area_width // width
                bottom = (r["y"] + r["height"] - top) * area_height // height
                scaled.append(dict(x=x, y=y, width=right - x, height=bottom - y))
            return peer, scaled

        def draw_layout(self):
            self.updating_layout = True
            self.scene.clear(); self.groups.clear(); self.contact_lines.clear(); self.separation_items.clear()
            for i in range(7):
                if not self.table.cellWidget(i, 1).isChecked():
                    continue
                peer, screens = self.station_screens(i)
                width = max(r["x"] + r["width"] for r in screens)
                height = max(r["y"] + r["height"] for r in screens)
                group = StationItem(width, height, lambda item, row=i: self.station_moved(row, item),
                                    self.drag_finished, lambda pos, row=i: self.snap_station(row, pos),
                                    lambda row=i: self.table.setCurrentCell(row, 0))
                self.scene.addItem(group)
                for j, r in enumerate(screens):
                    text = f"Dongle {i + 1} · screen {j + 1}" if peer.get("screens") else f"Dongle {i + 1}"
                    rect = ScreenItem((r["x"], r["y"], r["width"], r["height"]), text)
                    rect.setBrush(QColor("#326c9e" if peer.get("eligible") else "#68717a"))
                    pen = QPen(QColor(EDGE_STYLES[0][1]), 3); pen.setCosmetic(True); rect.setPen(pen)
                    if not peer.get("screens"):
                        rect.setToolTip(f"Dongle {i + 1}: no reported screens; showing a preview of the configured area")
                    group.addToGroup(rect)
                    group.screens.append(rect.rect())
                self.groups[i] = group
                group.setPos(self.stations[i]["x"], self.stations[i]["y"])
            self.updating_layout = False
            self.select_station(self.table.currentRow())
            self.view.fit_layout()

        def station_moved(self, i, group):
            if self.updating_layout:
                return
            self.stations[i].update(x=round(group.pos().x()), y=round(group.pos().y()))
            self.layout_moved = True
            self.mark_dirty()

        def snap_station(self, row, position):
            if self.updating_layout or row not in self.groups:
                return position
            neighbors = [rect for i, group in self.groups.items() if i != row for rect in group.placed_screens()]
            tolerance = max(8, 14 / max(.001, abs(self.view.transform().m11())))
            self.table.setCurrentCell(row, 0)
            return snap_position(self.groups[row].screens, position, neighbors, tolerance)

        def drag_finished(self):
            if self.deferred_peers is not None:
                peers, revision = self.deferred_peers
                self.deferred_peers = None
                # Release the mouse grab before replacing its graphics item.
                QTimer.singleShot(0, lambda: self.loaded_peers(peers, revision))
            self.view.fit_layout()

        def select_station(self, row, *_):
            for i, group in self.groups.items():
                group.setSelected(i == row)
                group.setZValue(1 if i == row else 0)
            self.show_contacts(row)

        def show_contacts(self, _row):
            for item in (*self.contact_lines, *self.separation_items):
                self.scene.removeItem(item)
            self.contact_lines.clear()
            self.separation_items.clear()
            placed = {i: group.placed_screens() for i, group in self.groups.items()}
            screens = [(i, rect) for i, group in placed.items() for rect in group]
            for kind, distance, position, area in desk_separations(screens):
                if area is not None:
                    hatch = self.scene.addRect(area, QPen(Qt.PenStyle.NoPen), overlap_brush())
                    hatch.setZValue(1.5)
                    hatch.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
                    self.separation_items.append(hatch)
                self.separation_items.append(desk_label(self.scene, position, f"{distance} px {kind}"))

            revision = self.document["revision"]
            authoritative = any("crossings" in peer for peer in self.peers)
            current = [peer for peer in self.peers if peer.get("layout_revision") == revision]
            if self.dirty and authoritative:
                self.crossing_status.setText("Save the layout to see its active crossings.")
                return
            if authoritative:
                self.crossing_status.setText("" if current else "Waiting for the controller's crossing map.")
                for peer in current:
                    if not peer["eligible"]:
                        continue
                    for crossing in peer.get("crossings", ()):
                        line = QLineF(*crossing["line"])
                        kind = crossing["kind"]
                        for pen in edge_pens(kind):
                            self.add_contact_line(line, pen).setToolTip(
                                f"{EDGE_STYLES[kind][0]} to dongle {crossing['target']}")
                        if kind == 4:
                            arrow = ("\u2190", "\u2192", "\u2191", "\u2193")[crossing["direction"]]
                            self.separation_items.append(desk_label(
                                self.scene, line.pointAt(.5), f"{arrow} {crossing['target']}"))
                return

            self.crossing_status.setText("Layout preview. Full active crossings require updated controller firmware.")
            if self.global_enabled.isChecked():
                for i, group in placed.items():
                    for j, neighbors in placed.items():
                        if j <= i:
                            continue
                        for contact in shared_edges(group, neighbors):
                            self.add_contact_line(contact, edge_pens(1)[0])
            if not self.dirty:
                for peer in current:
                    if peer["eligible"]:
                        for contact in peer.get("remembered_edges", ()):
                            self.add_contact_line(QLineF(*contact), edge_pens(3)[0]).setToolTip("Remembered crossing")

        def add_contact_line(self, contact, pen):
            line = self.scene.addLine(contact, pen)
            line.setZValue(2)
            line.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
            self.contact_lines.append(line)
            return line

        def toggle_station(self, row, checked):
            if checked:
                from PySide6.QtCore import QRectF
                _, screens = self.station_screens(row)
                width = max(r["x"] + r["width"] for r in screens)
                height = max(r["y"] + r["height"] for r in screens)
                x, y = self.stations[row]["x"], self.stations[row]["y"]
                occupied = [group.area.translated(group.pos()) for group in self.groups.values()]
                candidates = [(x, y)]
                for bounds in occupied:
                    candidates.extend([(round(bounds.right()) + 40, y), (x, round(bounds.bottom()) + 40),
                                       (round(bounds.left() - width) - 40, y), (x, round(bounds.top() - height) - 40)])
                for x, y in candidates:
                    if -100000 <= x <= 100000 and -100000 <= y <= 100000 and not any(
                            QRectF(x, y, width, height).intersects(bounds) for bounds in occupied):
                        self.stations[row].update(x=x, y=y)
                        break
            self.mark_dirty()
            self.draw_layout()
            self.table.setCurrentCell(row, 0)

        def mark_dirty(self, *_):
            if self.updating_layout:
                return
            self.dirty = True
            self.edit_generation += 1
            if self.document:
                self.note.setText("Unsaved layout changes")
            self.show_contacts(self.table.currentRow())
            if not self.scene.mouseGrabberItem():
                self.view.fit_layout()

        def load_preferences(self):
            self.submit("preferences-get", callback=self.edit_preferences)

        def edit_preferences(self, doc):
            from .preferences import LIMITS, TRIGGERS, COLOURS, LABELS
            if self.device_form is not None:
                self.device_box.removeWidget(self.device_form)
                self.device_form.deleteLater()
            self.device_form = QWidget()
            self.device_box.insertWidget(2, self.device_form)
            self.device_note.setText("Device preferences are stored on the master. Save changes below.")
            form = QFormLayout(self.device_form); fields = {}
            for name, value in doc["values"].items():
                if name == "slot_colors":
                    for i, color in enumerate(value):
                        combo = QComboBox()
                        for number, (label, rgb) in enumerate(COLOURS):
                            swatch = QPixmap(16, 16); swatch.fill(QColor(rgb))
                            combo.addItem(QIcon(swatch), label, number)
                        combo.setCurrentIndex(combo.findData(color))
                        fields[f"color_{i}"] = combo; form.addRow(f"Dongle {i + 1} colour", combo)
                elif name == "trigger":
                    combo = QComboBox()
                    for number, label in enumerate(TRIGGERS[1:], 1): combo.addItem(label, number)
                    combo.setCurrentIndex(combo.findData(value))
                    fields[name] = combo; form.addRow("Switching trigger", combo)
                elif name == 'boot_slot':
                    combo = QComboBox(); combo.addItem('Automatic (first available)', 0)
                    for slot in range(1, 8): combo.addItem(f'Dongle {slot}', slot)
                    combo.setCurrentIndex(combo.findData(value))
                    fields[name] = combo; form.addRow(LABELS[name], combo)
                elif type(value) is bool:
                    check = QCheckBox(); check.setChecked(value); fields[name] = check
                    form.addRow(LABELS[name], check)
                else:
                    spin = QSpinBox(); spin.setRange(*LIMITS[name]); spin.setValue(value); fields[name] = spin
                    form.addRow(LABELS[name], spin)
            save = QPushButton("Save preferences")
            def commit():
                values = dict(doc["values"])
                for name, field in fields.items():
                    if name.startswith("color_"):
                        continue
                    values[name] = field.isChecked() if isinstance(field, QCheckBox) else field.currentData() if isinstance(field, QComboBox) else field.value()
                values["slot_colors"] = [fields[f"color_{i}"].currentData() for i in range(7)]
                def saved(result):
                    doc["revision"] = result["revision"]
                    self.device_note.setText("Device preferences saved")
                self.submit("preferences-set", callback=saved, values=values, revision=doc["revision"])
            save.clicked.connect(commit); form.addRow(save)
            reset = QPushButton('Restore preference defaults…')
            def restore():
                if QMessageBox.question(self, 'Restore device preferences?',
                    'Restore the everyday preferences to firmware defaults? Pairings, USB input choices and history are retained.',
                    defaultButton=QMessageBox.StandardButton.No) == QMessageBox.StandardButton.Yes:
                    self.submit('preferences-default', callback=self.edit_preferences, revision=doc['revision'])
            reset.clicked.connect(restore); form.addRow(reset)

        def current_document(self):
            doc = copy.deepcopy(self.document)
            c = doc["config"]
            try:
                gap, overlap = int(self.gap_tolerance.text()), int(self.overlap_tolerance.text())
            except ValueError as error:
                raise ValueError("Enter a non-negative whole number for each join tolerance") from error
            c.update(enabled=self.global_enabled.isChecked(), delay_ms=self.delay.value(), corner=self.corner.value(),
                     gap_tolerance=gap, overlap_tolerance=overlap)
            for s in c["stations"]:
                i = s["slot"] - 1
                s.update(self.stations[i])
                s["configured"] = self.table.cellWidget(i, 1).isChecked()
                if s["configured"] and (not s["width"] or not s["height"]):
                    peer = next((p for p in self.peers if p["slot"] == s["slot"]), {})
                    screens = peer.get("screens", [])
                    if screens:
                        s["width"] = s["width"] or min(32767, max(r["x"] + r["width"] for r in screens) - min(r["x"] for r in screens))
                        s["height"] = s["height"] or min(32767, max(r["y"] + r["height"] for r in screens) - min(r["y"] for r in screens))
            return validate_document(doc)

        def save(self):
            try:
                doc = self.current_document()
                if doc["revision"] is None:
                    raise RuntimeError("This is an offline draft. Export it, connect a receiver, Reload, then Import the draft before saving.")
                if any(future.kms_command == "config-set" for future, _ in self.pending):
                    return
                generation = self.edit_generation
                def saved(result):
                    if generation == self.edit_generation:
                        self.loaded(result)
                        self.note.setText("Layout saved to master.")
                    else:
                        if self.document["revision"] == doc["revision"]:
                            self.document = validate_document(result)
                        self.note.setText("Layout saved to master; newer edits still need saving.")
                options = {"refresh_joins": True} if self.layout_moved else {}
                self.submit("config-set", callback=saved, **doc, **options)
            except (RuntimeError, ValueError) as error:
                self.report_error("Layout not saved", error)

        def export_config(self):
            name, _ = QFileDialog.getSaveFileName(self, "Export layout", "kmswitcher-layout.json", "JSON (*.json)")
            if name:
                try:
                    from pathlib import Path
                    from PySide6.QtCore import QIODevice, QSaveFile
                    path = Path(name)
                    if not path.suffix:
                        path = path.with_suffix(".json")
                    data = (json.dumps(self.current_document(), indent=2) + "\n").encode("utf-8")
                    output = QSaveFile(str(path))
                    if not output.open(QIODevice.OpenModeFlag.WriteOnly):
                        raise OSError(output.errorString())
                    if output.write(data) != len(data) or not output.commit():
                        output.cancelWriting()
                        raise OSError(output.errorString())
                    self.note.setText(f"Exported layout to {path.resolve()}")
                except (OSError, RuntimeError, ValueError) as error:
                    self.report_error("Layout not exported", error)

        def import_config(self):
            name, _ = QFileDialog.getOpenFileName(self, "Import layout", "", "JSON (*.json)")
            if name:
                try:
                    from pathlib import Path
                    doc = validate_document(json.loads(Path(name).read_text(encoding="utf-8-sig")))
                    if self.dirty and not self.confirm_discard():
                        return
                    if doc["revision"] is None:
                        # An offline draft has never had a master revision to preserve.
                        doc["revision"] = self.document["revision"]
                    # Preserve exported revision: saving an old file must require a reload/explicit CLI override.
                    self.loaded(doc); self.mark_dirty()
                    self.note.setText("Imported layout; review and Save to master. Stale revisions are rejected.")
                except (OSError, ValueError) as error:
                    self.report_error("Layout not imported", error)

        def report_error(self, title, error):
            self.note.setText(str(error))
            QMessageBox.warning(self, title, str(error))

        def confirm_discard(self):
            return QMessageBox.question(self, "Unsaved layout changes", "Discard the current layout edits?") == QMessageBox.StandardButton.Yes

        def open_settings(self):
            self.showNormal(); self.raise_(); self.activateWindow()

        def tray_activated(self, reason):
            if reason == QSystemTrayIcon.ActivationReason.Trigger:
                self.open_settings()
            elif sys.platform == "win32" and reason == QSystemTrayIcon.ActivationReason.Context:
                # Open the guarded Qt popup after Windows finishes delivering
                # the tray activation. A native menu can bypass its mouse handlers.
                QTimer.singleShot(0, self, self.show_tray_menu)

        def show_tray_menu(self):
            if not self.quitting and not self.tray_menu.isVisible():
                self.tray_menu.popup(QCursor.pos())

        def closeEvent(self, event):
            if self.quitting:
                event.accept()
            elif QSystemTrayIcon.isSystemTrayAvailable():
                event.ignore(); self.hide()
            else:
                # A desktop without tray support keeps a visible route to the controls.
                event.ignore(); self.showMinimized()

        def quit_companion(self, *, source="tray menu"):
            if self.dirty and QMessageBox.question(self, "Unsaved changes", "Discard unsaved layout edits and quit?") != QMessageBox.StandardButton.Yes:
                return
            logging.getLogger(__name__).info("Quit companion requested from %s. Stopping helper and tray.", source)
            self.device_stop.set()
            try:
                ipc.call("stop")
            except RuntimeError:
                pass
            self.quitting = True
            self.tray.hide(); app.quit()

    try:
        window = Window()
    except Exception:
        if instance:
            instance.close()
        raise
    if instance:
        def show_existing(message):
            if message == b"show":
                window.open_settings()
            elif message == b"quit":
                window.quit_companion(source="local quit command")
        instance.receive(show_existing)
    if not tray_only or not QSystemTrayIcon.isSystemTrayAvailable():
        window.show()
    if execute:
        try:
            return app.exec()
        finally:
            window.device_stop.set()
            window.timer.stop()
            window.tray.hide()
            window.pool.shutdown(wait=True, cancel_futures=True)
            window.transfer_pool.shutdown(wait=True, cancel_futures=True)
            window.device_pool.shutdown(wait=True, cancel_futures=True)
            instance.close()
    return window
