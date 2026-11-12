"""Inspect and export retained diagnostics from any paired receiver."""
import json
from pathlib import Path
from queue import Empty, SimpleQueue
from threading import Event

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QComboBox, QFileDialog, QHBoxLayout, QLabel, QMessageBox, QPlainTextEdit,
                               QPushButton, QVBoxLayout, QWidget)

from .remote_diagnostics import RemoteDiagnostics


class RemoteDiagnosticsPanel(QWidget):
    def __init__(self, background):
        super().__init__()
        self.background = background
        self.capture, self.progress, self.cancelled = None, SimpleQueue(), Event()
        layout = QVBoxLayout(self)
        self.note = QLabel('Read a paired dongle’s status or retained history through the controller.'); self.note.setWordWrap(True)
        layout.addWidget(self.note)
        row = QHBoxLayout(); layout.addLayout(row)
        self.slot = QComboBox()
        for slot in range(1, 8): self.slot.addItem(f'Dongle {slot}', slot)
        row.addWidget(self.slot)
        self.controls = [self.slot]
        for name, action in (('Inspect', self.inspect), ('Download history…', self.history), ('Clear retained history…', self.clear)):
            button = QPushButton(name); button.clicked.connect(action); row.addWidget(button); self.controls.append(button)
        self.export = QPushButton('Export status JSON…'); self.export.clicked.connect(self.export_status); self.export.setEnabled(False)
        row.addWidget(self.export)
        self.cancel = QPushButton('Cancel read'); self.cancel.clicked.connect(self.cancelled.set); self.cancel.setEnabled(False)
        row.addWidget(self.cancel); row.addStretch()
        self.output = QPlainTextEdit(); self.output.setReadOnly(True); layout.addWidget(self.output, 1)
        self.timer = QTimer(self); self.timer.timeout.connect(self.show_progress)
        self.slot.currentIndexChanged.connect(self.clear_view)

    def clear_view(self):
        self.capture = None; self.output.clear(); self.export.setEnabled(False)

    def set_busy(self, busy, cancellable=False):
        for control in self.controls: control.setEnabled(not busy)
        self.export.setEnabled(not busy and self.capture is not None)
        self.cancel.setEnabled(busy and cancellable)
        if busy and cancellable: self.timer.start(200)
        else: self.timer.stop()

    def show_progress(self):
        latest = None
        try:
            while True: latest = self.progress.get_nowait()
        except Empty:
            if latest:
                done, total = latest; self.note.setText(f'Reading history: {done:,} of {total:,} bytes…')

    def run(self, message, work, finished, *, cancellable=False):
        slot = self.slot.currentData(); self.cancelled.clear(); self.progress = SimpleQueue()
        self.note.setText(message); self.set_busy(True, cancellable)

        def done(result):
            self.set_busy(False); finished(slot, result)

        def failed(error):
            self.set_busy(False); self.note.setText(str(error))

        self.background(lambda call: work(RemoteDiagnostics(call, slot, cancelled=self.cancelled.is_set)), done, failed)

    def inspect(self):
        self.clear_view()
        def show(slot, result):
            self.capture = result; self.output.setPlainText(json.dumps(result, indent=2))
            self.export.setEnabled(True); self.note.setText(f'Dongle {slot} status read successfully.')
        self.run('Inspecting dongle…', lambda device: device.status(), show)

    def history(self):
        def save(slot, data):
            value, _ = QFileDialog.getSaveFileName(self, 'Save retained history', f'dongle-{slot}-history.txt', 'Text (*.txt)')
            if not value:
                self.note.setText('History read successfully; no file saved.'); return
            try:
                Path(value).write_bytes(data); self.note.setText(f'Verified history saved ({len(data):,} bytes).')
            except OSError as error: self.note.setText(str(error))
        self.run('Reading retained history…', lambda device: device.history(lambda done, total: self.progress.put((done, total))),
                 save, cancellable=True)

    def clear(self):
        slot = self.slot.currentData()
        if QMessageBox.question(self, f'Clear dongle {slot} history?',
            'This removes retained events and saved checkpoints. Pairing and live counters are retained.',
            defaultButton=QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes: return
        def done(slot, _):
            self.clear_view(); self.note.setText(f'Dongle {slot} retained history cleared.')
        self.run('Clearing retained history…', lambda device: device.clear(), done)

    def export_status(self):
        if not self.capture: return
        value, _ = QFileDialog.getSaveFileName(self, 'Export receiver status', f"dongle-{self.capture['slot']}-status.json", 'JSON (*.json)')
        if value:
            try: Path(value).write_text(json.dumps(self.capture, indent=2) + '\n', encoding='utf-8')
            except OSError as error: self.note.setText(str(error))
