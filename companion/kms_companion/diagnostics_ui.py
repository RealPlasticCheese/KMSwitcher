"""Local retained-event viewer; no device access on the Qt thread."""
import json


def create_dialog(parent, refresh):
    from PySide6.QtWidgets import (QDialog, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
        QLineEdit, QTableWidget, QTableWidgetItem, QFileDialog, QMessageBox)
    from PySide6.QtCore import Qt

    class DiagnosticsDialog(QDialog):
        def __init__(self):
            super().__init__(parent)
            self.setWindowTitle('Local dongle logs'); self.resize(950, 540)
            self.state = {}; self.rows = []
            box = QVBoxLayout(self)
            self.summary = QLabel('Read retained diagnostic events directly from this computer’s S2 over USB.')
            self.summary.setWordWrap(True); box.addWidget(self.summary)
            controls = QHBoxLayout()
            self.filter = QLineEdit(); self.filter.setPlaceholderText('Filter by boot, event or detail')
            self.filter.textChanged.connect(self.render_rows); controls.addWidget(self.filter)
            self.refresh = QPushButton('Refresh'); self.refresh.clicked.connect(refresh); controls.addWidget(self.refresh)
            self.export = QPushButton('Export…'); self.export.clicked.connect(self.export_logs); self.export.setEnabled(False)
            controls.addWidget(self.export); box.addLayout(controls)
            self.table = QTableWidget(0, 5)
            self.table.setHorizontalHeaderLabels(['Boot', 'First uptime', 'Last uptime', 'Count', 'Event / detail'])
            self.table.horizontalHeader().setStretchLastSection(True)
            self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            box.addWidget(self.table)
            self.recoveries = QLabel('Recent recovery outcomes will appear here.')
            self.recoveries.setWordWrap(True); box.addWidget(self.recoveries)
            note = QLabel('Times are uptime within each boot. Refresh reads a frozen journal; older entries can be overwritten when the ring fills. No input contents are recorded.')
            note.setWordWrap(True); box.addWidget(note)

        def show_state(self, state):
            self.state = state; self.rows = state.get('entries', [])
            loading, error = state.get('loading', False), state.get('error', '')
            text = f"{len(self.rows)}/{state.get('count', '?')} events · {state.get('overwritten', 0)} overwritten · current boot {state.get('boot', '?')} · reset code {state.get('reset_reason', '?')}"
            if loading: text += ' · Reading…'
            if error: text += ' · Incomplete: ' + error
            self.summary.setText(text); self.refresh.setEnabled(not loading)
            self.export.setEnabled(not loading and not error and state.get('supported', False))
            messages = [e['message'] + f" (attempt {e['attempts']})" for e in state.get('recovery_events', [])[-5:]]
            self.recoveries.setText('Recent recovery outcomes:\n' + '\n'.join(messages) if messages else 'No new recovery outcomes observed in this companion session.')
            self.render_rows()

        def render_rows(self):
            needle = self.filter.text().casefold()
            rows = [r for r in self.rows if needle in (r['boot'] + ' ' + r['description'] + f" {r['detail']:08x}").casefold()]
            self.table.setRowCount(len(rows))
            for row, event in enumerate(rows):
                values = (event['boot'], f"{event['first_ms'] / 1000:.3f} s", f"{event['last_ms'] / 1000:.3f} s",
                          str(event['repeats']), event['description'] + f" · detail 0x{event['detail']:08x}")
                for col, value in enumerate(values): self.table.setItem(row, col, QTableWidgetItem(value))
            self.table.resizeColumnsToContents()

        def export_logs(self):
            if not self.export.isEnabled(): return
            path, _ = QFileDialog.getSaveFileName(self, 'Export local dongle logs', 'dongle-logs.json', 'JSON (*.json)')
            if path:
                from pathlib import Path
                try: Path(path).write_text(json.dumps(self.state, indent=2) + '\n', encoding='utf-8')
                except OSError as error: QMessageBox.warning(self, 'Logs not exported', str(error))

    return DiagnosticsDialog()
