"""Shared, on-demand maintenance window for regular devices and test fixtures."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import sys
import webbrowser

from . import config_directory, firmware, management, releases

_DEFAULT_PORT = object()


def launch(product="regular", bundle=None, *, execute=True, parent=None, initial_task=None):
    from PySide6.QtCore import QProcess, QTimer, Qt
    from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFileDialog, QHBoxLayout,
        QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPlainTextEdit,
        QProgressBar, QPushButton, QTabWidget, QVBoxLayout, QWidget)
    app = QApplication.instance() or QApplication(sys.argv[:1])

    class Window(QMainWindow):
        def __init__(self):
            super().__init__(parent)
            self.product = product
            self.bundle = Path(bundle) if bundle else firmware.default_bundle()
            self.application = self.application_info = self.operation_report = None
            if self.bundle.is_file(): self.bundle = releases.install_archive(self.bundle, product)
            self.busy = False
            self.pool = ThreadPoolExecutor(max_workers=1)
            self.future = None
            self.process = None
            self.web_dashboard = False
            self.devices = []
            self.stored_receiver = None
            self.stored_receiver_error = None
            self.inspected_port = None
            self.downloads = []
            self.controls = []
            self.setWindowTitle("KMSwitcher Stress Toolkit" if product == "stress" else "KMSwitcher Firmware Setup")
            self.resize(900, 760)
            central = QWidget(); self.setCentralWidget(central); box = QVBoxLayout(central)
            heading = QLabel(self.windowTitle()); heading.setStyleSheet("font-size: 24px; font-weight: bold")
            box.addWidget(heading)
            self.tabs = QTabWidget(); box.addWidget(self.tabs, 1)
            self.home = self.page("Start here")
            credit = self.label(self.home, 'By <a href="https://plasticcheese.com/">Frank Tzanabetis</a>')
            credit.setOpenExternalLinks(True)
            self.updates = self.page("Firmware")
            self.setup = self.page("Pair & configure" if product == "regular" else "Prepare test rig")
            self.sources = self.page("Get updates")
            self.restarts = self.page("Restart dongles")
            self.label(self.restarts, "Connect the S3 controller COM/UART socket to this computer. Leave dongles powered in their usual computers. Restart one dongle or all paired dongles. Pairings and settings are retained; input through a restarting dongle pauses briefly. Both S3 and S2 firmware must support remote restart.")
            row = QHBoxLayout(); self.restarts.addLayout(row)
            self.restart_port = QComboBox(); row.addWidget(self.restart_port, 1); self.controls.append(self.restart_port)
            self.button(row, "Refresh ports", self.refresh)
            self.button(self.restarts, "Inspect paired dongles", lambda: self.command("inspect", [], self.restart_inspected,
                        port=self.restart_port.currentData()))
            self.restart_target = QComboBox(); self.restarts.addWidget(self.restart_target); self.controls.append(self.restart_target)
            self.restart_port.currentIndexChanged.connect(self.clear_restart_targets)
            self.clear_restart_targets()
            self.button(self.restarts, "Restart selected dongle(s)", self.restart_devices)
            self.restarts.addStretch()
            self.label(self.home, "Choose a task. This tool runs only while open. Keyboard switching works without the optional companion.")
            for text in ("Set up a new device", "Update firmware", "Recover a device"):
                self.button(self.home, text, lambda checked=False, task=text: self.start_task(task))
            self.button(self.home, "Restart dongles", lambda: self.tabs.setCurrentWidget(self.restarts.parentWidget()))
            self.button(self.home, "Prepare test rig" if product == "stress" else "Pair and configure devices",
                        lambda: self.tabs.setCurrentIndex(2))
            if product == "stress":
                self.button(self.home, "Run stress tests", lambda: self.run_test([]))
                self.button(self.home, "Open browser dashboard", lambda: self.run_test(["--web", "--open-browser"]))
                self.stop_browser = QPushButton("Stop browser dashboard")
                self.home.addWidget(self.stop_browser); self.stop_browser.setEnabled(False)
                self.stop_browser.clicked.connect(lambda: self.process.terminate() if self.web_dashboard else None)
                self.button(self.home, "Open saved reports", self.open_reports)
                self.label(self.home, "Tests and firmware updates share a hardware lock. Close the test dashboard before updating firmware.")
            self.home.addStretch()

            self.bundle_label = self.label(self.updates, "")
            row = QHBoxLayout(); self.updates.addLayout(row)
            self.button(row, "Import firmware ZIP…", self.import_archive)
            self.button(row, "Choose firmware folder…", self.choose_folder)
            self.button(row, 'Choose local application…', self.choose_application)
            self.role = QComboBox()
            self.role.addItem("S3 controller", "controller"); self.role.addItem("S2 receiver / dongle", "receiver")
            if product == "stress": self.role.addItem("S3 test input generator", "generator")
            self.updates.addWidget(self.role); self.controls.append(self.role)
            self.method = QComboBox()
            self.method.addItem("Direct USB — initial installation, update or recovery", "usb")
            self.method.addItem("Application update through controller — receivers use OTA", "ota")
            self.updates.addWidget(self.method); self.controls.append(self.method)
            from kms_transport import PROFILES, PROFILE_LABELS
            self.speed = QComboBox()
            for value, label in zip(PROFILES, PROFILE_LABELS): self.speed.addItem(label, value)
            self.updates.addWidget(self.speed); self.controls.append(self.speed)
            self.receiver_source_label = self.label(self.updates, "Firmware source")
            self.receiver_source = QComboBox()
            self.receiver_source.addItem("Firmware from selected release", "bundle")
            self.receiver_source.addItem('Local application image and manifest', 'application')
            self.receiver_source.addItem("Receiver firmware stored on S3", "cache")
            self.updates.addWidget(self.receiver_source); self.controls.append(self.receiver_source)
            self.cache_label = self.label(self.updates, "Inspect the controller to read its stored S2 firmware.")
            self.instructions = self.label(self.updates, "")
            self.diagram = self.label(self.updates, "")
            self.diagram.setStyleSheet("background: #203344; color: white; padding: 14px; font-size: 15px")
            row = QHBoxLayout(); self.updates.addLayout(row)
            self.port = QComboBox(); row.addWidget(self.port, 1); self.controls.append(self.port)
            self.button(row, "Refresh ports", self.refresh)
            self.inspect_button = self.button(self.updates, "Inspect controller and paired dongles", lambda: self.command("inspect", [], self.inspected))
            self.list = QListWidget(); self.updates.addWidget(self.list); self.controls.append(self.list)
            self.add_device_choice(0, "Controller: select to flash the connected S3")
            self.force = QCheckBox("OTA: reinstall even if the application image already matches")
            self.updates.addWidget(self.force); self.controls.append(self.force)
            self.update_button = self.button(self.updates, "Review and update selected device(s)", self.update_devices)
            self.erase_button = self.button(self.updates, "Erase && Flash…", self.erase_and_flash)
            self.cache_button = self.button(self.updates, 'Store receiver firmware on S3…', self.store_receiver)
            self.role.currentIndexChanged.connect(self.connection_changed)
            self.method.currentIndexChanged.connect(self.connection_changed)
            self.receiver_source.currentIndexChanged.connect(self.source_changed)
            self.port.currentIndexChanged.connect(self.port_changed)
            self.make_setup()

            self.label(self.sources, "Import a firmware ZIP to work offline, or select a release from Get updates. Firmware and this application update independently.")
            self.source = releases.default_source()
            self.source_file = config_directory() / f"{product}-release-feed.txt"
            self.feed = QLineEdit(self.source_file.read_text().strip() if self.source_file.exists() else "")
            self.feed.setPlaceholderText("Optional public HTTPS release-index.json address")
            self.sources.addWidget(self.feed); self.controls.append(self.feed)
            self.development = QCheckBox("Include development builds")
            self.sources.addWidget(self.development); self.controls.append(self.development)
            self.button(self.sources, "Check for updates", self.check_updates)
            self.release_list = QListWidget(); self.sources.addWidget(self.release_list)
            self.button(self.sources, "Download selected firmware", self.download_selected)
            browser_label = "Open private Azure releases" if isinstance(self.source, dict) else "Open GitHub releases"
            self.button(self.sources, browser_label, self.open_releases)
            self.button(self.sources, "Import downloaded firmware ZIP…", self.import_archive)
            self.sources.addStretch()
            self.progress = QProgressBar(); self.progress.setRange(0, 1); box.addWidget(self.progress)
            self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setMaximumHeight(180)
            self.log.setMaximumBlockCount(2000); box.addWidget(self.log)
            row = QHBoxLayout(); box.addLayout(row)
            self.button(row, "Save operation log…", self.save_log)
            self.report_button = QPushButton('Save results JSON…')
            self.report_button.setEnabled(False); self.report_button.clicked.connect(self.save_report); row.addWidget(self.report_button)
            self.clear_log_button = QPushButton("Clear log")
            self.clear_log_button.clicked.connect(self.log.clear); row.addWidget(self.clear_log_button)
            self.timer = QTimer(self); self.timer.timeout.connect(self.poll); self.timer.start(100)
            self.refresh(); self.connection_changed(); self.show_bundle()
            if initial_task == "restart": self.tabs.setCurrentWidget(self.restarts.parentWidget())

        def page(self, title):
            widget = QWidget(); layout = QVBoxLayout(widget); self.tabs.addTab(widget, title); return layout

        def label(self, layout, text):
            label = QLabel(text); label.setWordWrap(True); layout.addWidget(label); return label

        def button(self, layout, text, callback):
            button = QPushButton(text); button.clicked.connect(callback); layout.addWidget(button)
            self.controls.append(button); return button

        def make_setup(self):
            if product == "regular":
                self.label(self.setup, "Pair a new dongle:\n1. Power the controller and connect its keyboard/mouse hub.\n2. Connect the S2 normally to its computer.\n3. Tap Ctrl twice, then P, then a receiver number 1–7.\n4. Within 30 seconds, hold the S2 button for about three seconds and release.\n5. Tap Ctrl twice and that number to select it.\n\nUse your configured trigger if it differs from Ctrl. Existing pairings survive supported updates.")
                self.label(self.setup, 'Use Companion → Devices or Radio for everyday settings. The Engineering Console provides a live terminal through the controller UART. Disconnect the console before using the same port here.')
                self.button(self.setup, 'Open Engineering Console…', self.open_console)
                self.button(self.setup, 'Locate Engineering Console…', lambda: self.open_console(configure=True))
                self.label(self.setup, "Automatic screen-edge switching is optional. Install the separate Companion on each participating computer and arrange your desk there. This maintenance tool does not enable login startup.")
            else:
                self.label(self.setup, "1. Install / update the controller, receivers and each generator from Firmware.\n2. Pair the receivers with the controller.\n3. Connect every participating board's UART to this Linux computer.\n4. For a direct-generator baseline, connect generator native USB directly to Linux.\n5. After baseline passes, move generator native USB to the controller's powered hub. Leave receiver native USB connected to Linux for capture.\n6. Discover devices in the test dashboard, review identities and slots, then save the profile.")
                self.label(self.setup, "Baseline:  Generator USB → Linux capture\nRouted test:  Generator USB → Hub → S3 controller ⇢ S2 receiver USB → Linux\nControl:  Each board UART → Linux")
                self.button(self.setup, "Discover fixture devices", lambda: self.run_test(["--discover"]))
                self.button(self.setup, "Open test dashboard", lambda: self.run_test([]))
                self.label(self.setup, "Start with Baseline, then Quick. Use All tests / Soak for longer runs. The dashboard shows duration, device assignments, permission failures, progress and report links. Demo creates a labelled hardware-free report.")
            self.setup.addStretch()

        def start_task(self, task):
            self.tabs.setCurrentIndex(1)
            self.method.setCurrentIndex(1 if task == "Update firmware" else 0)

        def connection_changed(self):
            role, method = self.role.currentData(), self.method.currentData()
            self.method.setItemText(1, "OTA through controller — leave dongles connected" if role == "receiver"
                                    else "Update checked devices through controller UART")
            if role == "generator" and method == "ota":
                self.method.setCurrentIndex(0); return
            if method == "ota":
                self.diagram.setText("Updating computer  -- USB -->  S3 COM/UART  ... wireless ...  S2 dongle(s)")
            elif role == "receiver":
                self.instructions.setText("Unplug the S2. Hold its button while plugging it into this computer, then release. Refresh ports and select its programming port. No controller is required. Reconnect normally after verification.")
                self.diagram.setText("Updating computer  -- USB -->  S2 dongle  [hold button while connecting]")
            else:
                self.instructions.setText("Connect the S3 COM/UART socket with a data cable. If needed, hold BOOT, press and release RESET, then release BOOT. Select its UART port. Keep the native USB connection separate.")
                if role == "generator":
                    self.instructions.setText("Use only the spare S3 assigned as an input generator. This replaces controller functionality on the selected S3. " + self.instructions.text())
                self.diagram.setText("Updating computer  -- USB -->  S3 COM/UART  [BOOT + RESET if needed]")
            self.source_changed()

        def uses_stored_receiver(self):
            return self.method.currentData() == 'ota' and self.receiver_source.currentData() == 'cache'

        def uses_application(self):
            return self.method.currentData() == 'ota' and self.receiver_source.currentData() == 'application'

        def source_changed(self):
            through_controller = self.method.currentData() == 'ota'
            self.inspect_button.setVisible(through_controller)
            self.list.setVisible(through_controller)
            self.force.setVisible(through_controller)
            self.erase_button.setVisible(not through_controller)
            self.update_button.setText("Review and update selected device(s)" if through_controller
                                       else "Review and flash connected device")
            self.receiver_source_label.setVisible(through_controller)
            self.receiver_source.setVisible(through_controller)
            self.cache_label.setVisible(self.uses_stored_receiver())
            self.cache_button.setVisible(through_controller and not self.uses_stored_receiver() and
                                         (not self.uses_application() or (self.application_info or {}).get('role') == 'receiver'))
            self.show_bundle()
            if self.uses_stored_receiver():
                self.instructions.setText("Use verified S2 firmware already stored on the S3. Connect the controller COM/UART socket, inspect, then tick the dongles to update. No receiver firmware is uploaded from this computer. The controller is not updated.")
            elif through_controller:
                self.instructions.setText("Connect the controller COM/UART socket. Leave S2 dongles powered and paired in their usual computers. Inspect, tick each device to update, then review. Only checked devices will be updated; tick the controller only if you want to update it too. Older layouts require direct USB first.")
            self.render_devices()

        def port_changed(self):
            if self.inspected_port is None or self.port.currentData() == self.inspected_port:
                return
            self.inspected_port = None
            self.stored_receiver = None
            self.stored_receiver_error = None
            self.devices = []
            self.list.clear()
            self.add_device_choice(0, "Controller: select to flash the connected S3")
            self.source_changed()

        def show_bundle(self):
            if self.uses_stored_receiver():
                self.bundle_label.setText("Using S2 firmware stored on the connected S3. Local release files are not used.")
                return
            if self.uses_application():
                info = self.application_info
                self.bundle_label.setText(f"Local {info['role']} application · {info['git'][:12]}\n{self.application}"
                                          if info else 'Choose an application .bin with its matching .json manifest beside it.')
                return
            try:
                info = firmware.Bundle(self.bundle, product=product).validate()
                self.bundle_label.setText(f"Firmware {info['version']} · {info['commit'][:12]} · {product}\n{self.bundle}")
            except (OSError, ValueError, KeyError) as error:
                self.bundle_label.setText(f"Import a {product} firmware ZIP from Get updates, or choose an extracted bundle folder.")

        def choose_folder(self):
            value = QFileDialog.getExistingDirectory(self, "Choose folder containing bundle.json")
            if value:
                try:
                    firmware.Bundle(value, product=product).validate()
                    self.bundle = Path(value); self.receiver_source.setCurrentIndex(0); self.show_bundle()
                except (OSError, ValueError, KeyError) as error: self.failure(error)

        def import_archive(self):
            value, _ = QFileDialog.getOpenFileName(self, "Import firmware release", filter="Firmware archive (*.zip)")
            if value: self.background(lambda: releases.install_archive(value, product), self.adopt_bundle)

        def adopt_bundle(self, path):
            self.bundle = Path(path); self.receiver_source.setCurrentIndex(0); self.show_bundle(); self.tabs.setCurrentIndex(1)
            self.log.appendPlainText("Firmware verified and ready. Review the selected device and update method.")

        def choose_application(self):
            value, _ = QFileDialog.getOpenFileName(self, 'Choose an application image with its matching manifest', filter='Firmware application (*.bin)')
            if not value: return
            path = Path(value)
            def verify():
                role = json.loads(path.with_suffix('.json').read_text(encoding='utf-8')).get('role')
                if role not in ('controller', 'receiver'): raise ValueError('Choose a controller or receiver application image')
                return management.load_firmware(path, role)[1]
            def adopt(info):
                self.application, self.application_info = path, info
                self.role.setCurrentIndex(self.role.findData(info['role']))
                self.method.setCurrentIndex(self.method.findData('ota'))
                self.receiver_source.setCurrentIndex(self.receiver_source.findData('application'))
                self.source_changed()
            self.background(verify, adopt)

        def store_receiver(self):
            if self.busy or not self.port.currentData():
                if not self.busy: self.failure('Select the controller’s serial port')
                return
            try:
                arguments = []
                if self.uses_application():
                    if self.application is None: raise ValueError('Choose a local receiver application first')
                    _, info = management.load_firmware(self.application, 'receiver')
                    arguments = ['--application', str(self.application)]
                else: _, info = firmware.Bundle(self.bundle, product=product).application('receiver')
                if QMessageBox.question(self, 'Store receiver firmware?',
                    f"Store receiver firmware {info['git'][:12]} on {self.port.currentData()}? No dongles will be updated.",
                    defaultButton=QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes: return
                def stored(result):
                    self.inspected_port = self.port.currentData()
                    self.stored_receiver = result['receiver_cache']; self.stored_receiver_error = None
                    self.log.appendPlainText('Receiver firmware verified and stored on the controller.')
                    self.render_devices()
                self.command('store-receiver', arguments, stored)
            except (OSError, ValueError, KeyError, management.ManagementError) as error: self.failure(error)

        def refresh(self):
            ports = firmware.ports()
            for control in (self.port, self.restart_port):
                previous = control.currentData(); control.clear(); control.addItem("Select a serial port", None)
                for item in ports: control.addItem(f"{item['port']} — {item['description']}", item["port"])
                control.setCurrentIndex(max(0, control.findData(previous)))

        def clear_restart_targets(self):
            self.restart_target.clear()
            self.restart_target.addItem("Inspect paired dongles first", None)

        def restart_inspected(self, result):
            self.restart_target.clear()
            dongles = [row for row in result["devices"] if row["slot"]]
            if not dongles:
                self.clear_restart_targets(); self.log.appendPlainText("No paired dongles found."); return
            self.restart_target.addItem("Choose a dongle to restart", None)
            for row in dongles:
                detail = row.get("error") or ("ready" if row["identity"]["capabilities"] & 16 else "firmware update required")
                self.restart_target.addItem(f"Dongle {row['slot']} — {detail}", row["slot"])
            self.restart_target.addItem("All paired dongles (" + ", ".join(str(row['slot']) for row in dongles) + ")", "all")

        def restart_devices(self):
            target = self.restart_target.currentData()
            if target is None: self.failure("Inspect the controller and choose a dongle or all paired dongles"); return
            summary = "all paired dongles" if target == "all" else f"dongle {target}"
            if QMessageBox.question(self, "Restart dongles", f"Restart {summary} through the S3?\n\nInput through restarting dongles pauses briefly. Pairings and settings are retained. Offline or unsupported dongles will be reported individually.") != QMessageBox.StandardButton.Yes: return
            self.command("restart", ["--all"] if target == "all" else ["--slot", str(target)],
                         port=self.restart_port.currentData())

        def add_device_choice(self, slot, text):
            item = QListWidgetItem(text); item.setData(Qt.ItemDataRole.UserRole, slot)
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Unchecked)
            self.list.addItem(item)

        def inspected(self, result):
            self.inspected_port = self.port.currentData()
            self.devices = result["devices"]
            self.stored_receiver = result.get('receiver_cache')
            self.stored_receiver_error = result.get('receiver_cache_error')
            self.render_devices(preserve_checks=False)

        def render_devices(self, preserve_checks=True):
            stored = self.uses_stored_receiver()
            checked = {self.list.item(row).data(Qt.ItemDataRole.UserRole) for row in range(self.list.count())
                       if preserve_checks and self.list.item(row).checkState() == Qt.CheckState.Checked}
            if self.stored_receiver:
                image = self.stored_receiver
                self.cache_label.setText(f"Stored S2 firmware: {image['git']} · {image.get('size', 0):,} bytes\nSHA-256: {image['sha256']}")
            else:
                self.cache_label.setText(self.stored_receiver_error or
                    ("No verified S2 firmware is stored on this controller." if self.devices else
                     "Inspect the controller to read its stored S2 firmware."))
            if not self.devices:
                if self.list.count():
                    item = self.list.item(0)
                    if stored:
                        item.setCheckState(Qt.CheckState.Unchecked)
                        item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEnabled)
                    else: item.setFlags(item.flags() | Qt.ItemFlag.ItemIsEnabled)
                return
            self.list.clear()
            for device in self.devices:
                slot = device["slot"]; identity = device.get("identity", {})
                role = "controller" if slot == 0 else "receiver"
                expected = self.stored_receiver if stored and slot else None
                if self.uses_application():
                    expected = self.application_info if (self.application_info or {}).get('role') == role else None
                elif not stored:
                    try: expected = firmware.Bundle(self.bundle).application(role)[1]
                    except (OSError, ValueError, KeyError): pass
                matching = expected and identity.get('sha256') == expected['sha256']
                comparison = ('stored image' if matching else 'different from stored image') if stored else ('current image' if matching else 'different image')
                text = f"{'Controller' if slot == 0 else f'Dongle {slot}'}: " + (device.get("error") or f"{identity['git'][:12]} · {comparison} · {'update capable' if identity['capabilities'] & 2 else 'needs direct USB'}")
                if stored and slot == 0: text = "Controller: unchanged — stored S2 firmware updates dongles only"
                self.add_device_choice(slot, text)
                item = self.list.item(self.list.count() - 1)
                if (stored and slot == 0) or (self.uses_application() and expected is None):
                    item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsEnabled)
                elif slot in checked: item.setCheckState(Qt.CheckState.Checked)

        def erase_and_flash(self):
            port = self.port.currentData()
            role = self.role.currentData()
            try:
                bundle = firmware.Bundle(self.bundle, product=product)
                bundle.validate()
                bundle.files(role)
            except (OSError, ValueError, KeyError) as error:
                self.failure(error)
                return

            def confirm(identity):
                dialog = QMessageBox(self)
                dialog.setIcon(QMessageBox.Icon.Warning)
                dialog.setWindowTitle("Erase all saved settings and flash firmware?")
                dialog.setText(
                    f"This will erase all saved settings, receiver pairings, and existing firmware on "
                    f"{port} ({identity['chip']}, MAC {identity['mac']}), then install "
                    f"{bundle.document['version']}."
                )
                cancel = dialog.addButton(QMessageBox.StandardButton.Cancel)
                erase = dialog.addButton("Erase && Flash", QMessageBox.ButtonRole.DestructiveRole)
                dialog.setDefaultButton(cancel)
                dialog.setEscapeButton(cancel)
                dialog.exec()
                if dialog.clickedButton() is erase:
                    self.command("flash", ["--role", role, "--erase", "--expected-mac", identity["mac"]],
                                 port=port)

            self.command("flash", ["--role", role, "--identify"], confirm, port=port)

        def update_devices(self):
            try:
                stored = self.uses_stored_receiver()
                application = self.uses_application()
                bundle = None
                if not stored and not application:
                    bundle = firmware.Bundle(self.bundle, product=product); bundle.validate()
                role = self.role.currentData(); local = self.method.currentData() == "usb"
                checked = [self.list.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.list.count())
                           if self.list.item(i).checkState() == Qt.CheckState.Checked]
                selected = []
                if not local:
                    if not checked: raise ValueError("Inspect the controller and tick the devices to update")
                    if stored and (0 in checked or not self.stored_receiver):
                        raise ValueError("Inspect stored S2 firmware and select only dongles before updating")
                    role = "controller" if 0 in checked else "receiver"
                    selected = [slot for slot in checked if slot != 0]
                arguments = ["--role", role]
                if application:
                    if not self.application or (role == 'controller' and selected):
                        raise ValueError('Choose a local image and select only its device role')
                    _, info = management.load_firmware(self.application, role)
                    arguments += ['--application', str(self.application)]
                for slot in selected: arguments += ["--slot", str(slot)]
                if not local: arguments += ["--speed", self.speed.currentData()]
                if stored: arguments += ['--cached', '--cached-sha256', self.stored_receiver['sha256']]
                if not self.port.currentData(): raise ValueError("Select the connected device's serial port")
                targets = [role] if local else (["controller"] if 0 in checked else []) + [f"dongle {slot}" for slot in selected]
                if stored:
                    image_name = f"Stored S2 firmware {self.stored_receiver['git']}\nSHA-256: {self.stored_receiver['sha256']}\nNo receiver image will be uploaded to the controller."
                elif application: image_name = f"Local {role} firmware {info['git']}\nSHA-256: {info['sha256']}"
                else: image_name = f"Firmware {bundle.document['version']}"
                summary = f"{image_name}\nTarget: {', '.join(targets)}\nMethod: {'direct USB' if local else 'application update through controller'}\nPort: {self.port.currentData()}\n\nInput through updated devices will be interrupted. Supported updates preserve pairing/settings. Continue?"
                if not local and role == "controller" and selected:
                    summary += "\n\nThe selected dongles will be updated before the controller."
                if local and role == 'controller' and any(item['offset'] == 0x960000 for item in bundle.document['roles'][role]['files']):
                    summary += '\n\nThe S3 stored receiver firmware will also be refreshed.'
                if QMessageBox.question(self, "Review firmware update", summary) != QMessageBox.StandardButton.Yes: return
                if not local and self.force.isChecked(): arguments.append("--force")
                self.command("flash" if local else "ota", arguments)
            except (OSError, ValueError, KeyError, management.ManagementError) as error: self.failure(error)

        def command(self, name, arguments, callback=None, *, port=_DEFAULT_PORT):
            if self.busy: return
            selected_port = self.port.currentData() if port is _DEFAULT_PORT else port
            if not selected_port: self.failure("Select a serial port"); return
            if getattr(sys, "frozen", False):
                executable = str(Path(sys.executable).with_name("kms-firmware.exe" if os.name == "nt" else "kms-firmware")); prefix = []
            else: executable, prefix = sys.executable, ["-u", "-m", "kms_update"]
            self.start_process(executable, prefix + ["--product", product, "--bundle", str(self.bundle), name,
                "--port", selected_port, *arguments], callback)

        def start_process(self, executable, arguments, callback=None):
            if self.busy: return
            self.set_busy(True); self.output = bytearray(); self.process = QProcess(self)
            self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
            self.process.readyReadStandardOutput.connect(self.read_output)
            self.process.finished.connect(lambda code, status: self.finished(code, callback))
            self.process.errorOccurred.connect(lambda error: self.failed_start() if error == QProcess.ProcessError.FailedToStart else None)
            self.process.start(executable, arguments)

        def read_output(self):
            data = bytes(self.process.readAllStandardOutput()); self.output.extend(data)
            if len(self.output) > 2 * 1024 * 1024: del self.output[:-2 * 1024 * 1024]
            self.log.appendPlainText(data.decode("utf-8", errors="replace").replace("\r", "\n").rstrip())

        def failed_start(self):
            self.web_dashboard = False
            if product == "stress": self.stop_browser.setEnabled(False)
            self.set_busy(False); self.failure("Unable to start the packaged tool. Reinstall this release.")

        def finished(self, code, callback):
            self.read_output(); self.set_busy(False)
            self.web_dashboard = False
            if product == "stress": self.stop_browser.setEnabled(False)
            if not callback:
                try: result = json.loads(self.output.decode(errors='replace').strip().splitlines()[-1])
                except (ValueError, IndexError): result = {}
                if result.get('results'):
                    self.operation_report = result; self.report_button.setEnabled(True)
                for row in result.get("results", []):
                    target = "Controller" if row["slot"] == 0 else f"Dongle {row['slot']}"
                    self.log.appendPlainText(f"{target}: {row['result']}. {row.get('detail', '')}")
                    for warning in row.get("warnings", []): self.log.appendPlainText(warning)
                for warning in result.get("warnings", []): self.log.appendPlainText(warning)
                if (result.get("error") in (
                        "Update overlaps existing persistent data. Nothing was written.",
                        "Existing pairing layout differs. Nothing was written; use the recovery guide."
                ) and self.method.currentData() == "usb"):
                    self.log.appendPlainText("Use Erase & Flash if you want to erase all saved settings and install fresh.")
                if result.get("verified"):
                    self.log.appendPlainText("Firmware written and verified. Disconnect and reconnect this board normally, without holding BOOT.")
            if code: self.log.appendPlainText("Operation did not complete successfully. Review the per-device details above.")
            elif callback:
                try: callback(json.loads(self.output.decode().strip().splitlines()[-1]))
                except (ValueError, KeyError, OSError) as error: self.failure(error)
            else: self.log.appendPlainText("Operation completed. Review per-device results above.")

        def background(self, operation, callback):
            if self.busy: return
            self.set_busy(True); self.future = self.pool.submit(operation); self.future_callback = callback

        def poll(self):
            if self.future and self.future.done():
                future, self.future = self.future, None; self.set_busy(False)
                try: self.future_callback(future.result())
                except Exception as error: self.failure(error)

        def check_updates(self):
            source = self.feed.text().strip() or self.source
            if not source: self.failure("Enter a public release-index URL, or import a downloaded firmware ZIP."); return
            if isinstance(source, str):
                try:
                    releases.https_url(source); self.source_file.parent.mkdir(parents=True, exist_ok=True)
                    self.source_file.write_text(source)
                except (OSError, ValueError) as error: self.failure(error); return
            development = self.development.isChecked()
            self.background(lambda: releases.discover(source, product, development=development), self.found_releases)

        def found_releases(self, rows):
            self.downloads = rows; self.release_list.clear()
            for row in rows: self.release_list.addItem(f"{row['version']} · {'stable' if row.get('stable') else 'development'} · {row['commit'][:12]}")
            self.log.appendPlainText(f"Found {len(rows)} releases.")

        def download_selected(self):
            index = self.release_list.currentRow()
            if index < 0: self.failure("Select a release first"); return
            row = self.downloads[index]
            self.background(lambda: releases.download(row, product), self.adopt_bundle)

        def open_releases(self):
            if self.source and isinstance(self.source, dict):
                webbrowser.open(self.source["azure"] + "/_build?definitionId=" + str(self.source["definition"]))
            else: webbrowser.open(releases.PUBLIC_RELEASES)

        def open_console(self, *, configure=False):
            if not self.busy:
                from kms_transport.tools import open_console
                open_console(self, configure=configure)

        def run_test(self, arguments):
            self.web_dashboard = "--web" in arguments
            self.stop_browser.setEnabled(self.web_dashboard)
            if getattr(sys, "frozen", False):
                executable, prefix = str(Path(sys.executable).with_name("kms-stress")), []
            else: executable, prefix = sys.executable, ["-m", "kms_stress"]
            self.start_process(executable, prefix + arguments)

        def open_reports(self):
            from kms_stress.model import Config
            profile = Path.home() / ".config/kmswitcher-stress/profile.json"
            config = Config.load(profile) if profile.exists() else Config()
            path = Path(config.output).expanduser().resolve(); path.mkdir(parents=True, exist_ok=True)
            webbrowser.open(path.as_uri())

        def save_log(self):
            filename, _ = QFileDialog.getSaveFileName(self, "Save operation log", "kmswitcher-update.txt")
            if filename: Path(filename).write_text(self.log.toPlainText(), encoding="utf-8")

        def save_report(self):
            if not self.operation_report: return
            filename, _ = QFileDialog.getSaveFileName(self, 'Save operation results', 'kmswitcher-results.json', 'JSON (*.json)')
            if filename:
                try: Path(filename).write_text(json.dumps(self.operation_report, indent=2) + '\n', encoding='utf-8')
                except OSError as error: self.failure(error)

        def set_busy(self, value):
            self.busy = value
            for control in self.controls: control.setEnabled(not value)
            self.report_button.setEnabled(not value and self.operation_report is not None)
            self.progress.setRange(0, 0 if value else 1)

        def failure(self, error):
            self.log.appendPlainText(str(error))
            QMessageBox.information(self, "KMSwitcher", str(error))

        def closeEvent(self, event):
            if self.busy: event.ignore(); return
            self.pool.shutdown(wait=False); event.accept()

    window = Window()
    if os.environ.get("KMS_MAINTENANCE_SMOKE") == "1":
        if window.bundle.exists(): firmware.Bundle(window.bundle, product=product).validate()
        window.show(); app.processEvents(); window.close(); print("Maintenance UI ready"); return 0
    if not execute: return window
    window.show()
    if parent:
        # Retain this top-level window for the lifetime of its invoking settings window.
        parent.maintenance_window = window
        return 0
    return app.exec()
