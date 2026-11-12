"""Clipboard settings panel, independent of the tray and transfer polling."""
import socket

from PySide6.QtWidgets import (QComboBox, QFileDialog, QFormLayout, QHBoxLayout,
                               QCheckBox, QGridLayout, QLabel, QLineEdit, QPushButton,
                               QScrollArea, QSpinBox, QWidget)
from PySide6.QtCore import Qt

from .clipboard_preferences import (AUTOMATIC_MODES, CONTENT_LABELS, DEFAULT_RECEIVING,
                                    RECEIVE_LABELS, automatic_modes)
from .clipboard_protocol import MODES, MODE_LABELS
from kms_transport import PROFILES, PROFILE_LABELS


class ClipboardSettings(QWidget):
    def __init__(self, submit, parent=None):
        super().__init__(parent)
        self.submit = submit
        self.loaded = self.dirty = False
        form = QFormLayout(self)
        form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
        self.name = QLineEdit(socket.gethostname())
        self.mode, self.speed = QComboBox(), QComboBox()
        for value, label in zip(MODES, MODE_LABELS):
            self.mode.addItem(label, value)
        for value, label in zip(PROFILES, PROFILE_LABELS):
            self.speed.addItem(label, value)
        self.maximum = QSpinBox()
        self.maximum.setRange(1, 2048); self.maximum.setSuffix(" MB"); self.maximum.setValue(256)
        form.addRow("Computer name", self.name)
        form.addRow("Transfer via", self.mode)
        form.addRow("Dongle speed", self.speed)
        form.addRow("Maximum item size", self.maximum)
        types = QGridLayout()
        types.addWidget(QLabel("When receiving"), 0, 1)
        types.addWidget(QLabel("Send automatically"), 0, 2)
        types.setColumnStretch(1, 1)
        self.automatic = {}
        self.receiving = {}
        self.arrival_popups = {}
        for row, (kind, label) in enumerate(CONTENT_LABELS.items(), 1):
            combo = QComboBox()
            combo.setAccessibleName(f"When receiving {label.lower()}")
            for value, title in RECEIVE_LABELS.items():
                combo.addItem(title if kind != "files" or value != "clipboard" else "Prepare for paste automatically", value)
            combo.setCurrentIndex(combo.findData(DEFAULT_RECEIVING[kind]))
            self.receiving[kind] = combo
            automatic = QComboBox()
            automatic.setAccessibleName(f"Send {label.lower()} automatically")
            for mode, title in AUTOMATIC_MODES.items():
                automatic.addItem(title, mode)
            self.automatic[kind] = automatic
            popup = QCheckBox("Show arrival popup")
            popup.setAccessibleName(f"Show arrival popup for {label.lower()}")
            popup.toggled.connect(self.mark_dirty)
            self.arrival_popups[kind] = popup
            types.addWidget(QLabel(label), row, 0)
            types.addWidget(combo, row, 1)
            types.addWidget(automatic, row, 2)
            types.addWidget(popup, row, 3)
        form.addRow(types)
        self.targets = QScrollArea()
        self.targets.setFrameShape(QScrollArea.Shape.NoFrame)
        self.targets.setWidgetResizable(True)
        self.targets.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.targets.setAccessibleName("Selected computers for automatic clipboard sending")
        target_widget = QWidget()
        self.target_row = QHBoxLayout(target_widget)
        self.target_row.setContentsMargins(0, 0, 0, 0)
        self.targets.setWidget(target_widget)
        form.addRow("Selected computers", self.targets)
        self.known_computers = {}
        self.unknown_computers = {}
        self.selected_computers = {}
        def update_target_visibility():
            form.setRowVisible(self.targets, any(combo.currentData() == "selected" for combo in self.automatic.values()))
        for combo in self.automatic.values():
            combo.currentIndexChanged.connect(update_target_visibility)
        update_target_visibility()
        explanation = QLabel("Types set to Selected computers share the same destinations. "
                             "Only enable automatic sending to computers you trust with the content you copy.")
        explanation.setWordWrap(True); form.addRow(explanation)
        self.folder = QLineEdit()
        browse = QPushButton("Browse…")
        browse.clicked.connect(self.browse)
        folder_row = QHBoxLayout(); folder_row.addWidget(self.folder); folder_row.addWidget(browse)
        form.addRow("Automatic save folder", folder_row)
        hint = QLabel("Text and images are saved with a local date/time filename (.txt, .html, .rtf or .png). "
                      "All supported representations are kept. Files and folders keep their names inside a new KMSwitcher folder. "
                      "Transfers also offers Save to… for individual items.")
        hint.setWordWrap(True); form.addRow(hint)
        self.message = QLabel(); self.message.setWordWrap(True)
        self.save = QPushButton("Save clipboard settings")
        self.save.clicked.connect(self.commit)
        form.addRow(self.save); form.addRow(self.message)
        for field in (self.name, self.folder):
            field.textChanged.connect(self.mark_dirty)
        for combo in (self.mode, self.speed, *self.automatic.values(), *self.receiving.values()):
            combo.currentIndexChanged.connect(self.mark_dirty)
        self.maximum.valueChanged.connect(self.mark_dirty)
        self.load(dict(mode="network-preferred", speed="conservative", name=socket.gethostname(), max_mb=256))
        self.loaded = False

    def mark_dirty(self, *_):
        self.dirty = True
        self.message.setText("Unsaved clipboard settings")

    def load(self, settings):
        self.name.setText(settings["name"])
        self.mode.setCurrentIndex(self.mode.findData(settings["mode"]))
        self.speed.setCurrentIndex(self.speed.findData(settings.get("speed", "conservative")))
        self.maximum.setValue(settings["max_mb"])
        for kind, mode in automatic_modes(settings).items():
            combo = self.automatic[kind]
            combo.setCurrentIndex(combo.findData(mode))
        self.selected_computers = dict(settings.get("automatic_targets", {}))
        self.refresh_computers()
        for kind, combo in self.receiving.items():
            combo.setCurrentIndex(combo.findData(settings.get("receiving", {}).get(kind, DEFAULT_RECEIVING[kind])))
            self.arrival_popups[kind].setChecked(settings.get("arrival_popups", {}).get(kind, True))
        self.folder.setText(settings.get("save_folder", ""))
        self.loaded, self.dirty = True, False
        self.message.clear()

    def show_peers(self, peers, configured_slots):
        computers = {}
        unknown = {slot: ("", "Waiting for the companion") for slot in configured_slots}
        for peer in peers:
            slot, name = peer["slot"], peer.get("name", "")
            identifier = peer.get("machine_id")
            if identifier:
                computers[identifier] = (name, slot)
                unknown.pop(slot, None)
            else:
                unknown[slot] = (name, "Waiting for automatic clipboard discovery. Update the companion if needed.")
        if (computers, unknown) != (self.known_computers, self.unknown_computers):
            self.known_computers = computers
            self.unknown_computers = unknown
            self.refresh_computers()

    def refresh_computers(self):
        computers = {identifier: (name, None) for identifier, name in self.selected_computers.items()}
        computers.update(self.known_computers)
        while self.target_row.count():
            item = self.target_row.takeAt(0)
            if item.widget():
                item.widget().hide()
                item.widget().deleteLater()
        for identifier, (name, slot) in sorted(computers.items(), key=lambda entry: entry[1][0].casefold()):
            label = f"{name} (Dongle {slot})" if slot is not None else f"{name} (unavailable)"
            checkbox = QCheckBox(label)
            checkbox.setChecked(identifier in self.selected_computers)
            checkbox.toggled.connect(lambda checked, key=identifier, title=name: self.select_computer(key, title, checked))
            self.target_row.addWidget(checkbox)
        for slot, (name, reason) in sorted(self.unknown_computers.items()):
            label = f"{name} (Dongle {slot})" if name else f"Dongle {slot}"
            checkbox = QCheckBox(label)
            checkbox.setEnabled(False)
            checkbox.setToolTip(reason)
            self.target_row.addWidget(checkbox)
        if not self.known_computers:
            self.target_row.addWidget(QLabel("Waiting for computers. Start or update their companions."))
        self.target_row.addStretch()
        self.targets.setFixedHeight(self.target_row.sizeHint().height() + self.targets.horizontalScrollBar().sizeHint().height())

    def select_computer(self, identifier, name, checked):
        if checked:
            self.selected_computers[identifier] = name
        else:
            self.selected_computers.pop(identifier, None)
        self.mark_dirty()

    def browse(self):
        folder = QFileDialog.getExistingDirectory(self, "Choose automatic save folder", self.folder.text())
        if folder:
            self.folder.setText(folder)

    def commit(self):
        from .clipboard_service import TransferService
        try:
            settings = TransferService.validate_settings(self.values())
        except ValueError as error:
            self.message.setText(str(error))
            return
        def saved(result):
            # Retain edits made while the helper was saving.
            if settings == self.values():
                self.loaded = True
                self.dirty = False
                self.message.setText("Clipboard settings saved")
        self.submit("clipboard-settings", settings=settings, callback=saved)

    def values(self):
        return dict(name=self.name.text().strip(), mode=self.mode.currentData(), speed=self.speed.currentData(),
                    max_mb=self.maximum.value(), save_folder=self.folder.text().strip(),
                    automatic_send={kind: combo.currentData() for kind, combo in self.automatic.items()},
                    automatic_targets=dict(self.selected_computers),
                    arrival_popups={kind: popup.isChecked() for kind, popup in self.arrival_popups.items()},
                    receiving={kind: combo.currentData() for kind, combo in self.receiving.items()})
