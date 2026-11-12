"""Everyday controller controls, independent of the firmware updater."""
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QCheckBox, QComboBox, QHBoxLayout, QLabel, QMessageBox, QPushButton,
                               QTabWidget, QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from .device_control import JIGGLER_MODES
from .remote_diagnostics import usb_inventory


def button(row, title, callback):
    control = QPushButton(title); control.clicked.connect(callback); row.addWidget(control)
    return control


class DeviceManagement(QTabWidget):
    def __init__(self, preferences, background):
        super().__init__()
        self.background = background
        self.paired, self.jiggler, self.inventory = set(), None, None
        self.addTab(preferences, 'Preferences')

        dongles = QWidget(); box = QVBoxLayout(dongles)
        self.route_status = QLabel('Connect a dongle, then refresh.'); self.route_status.setWordWrap(True)
        box.addWidget(self.route_status)
        row = QHBoxLayout(); box.addLayout(row)
        button(row, 'Refresh dongles', self.refresh_dongles)
        button(row, 'Pause input', lambda: self.route(True))
        button(row, 'Resume input', lambda: self.route(False))
        row.addStretch()
        row = QHBoxLayout(); box.addLayout(row)
        self.slot = QComboBox()
        self.slot.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToContents)
        for slot in range(1, 8): self.slot.addItem(f'Dongle {slot}', slot)
        row.addWidget(self.slot)
        self.slot.currentIndexChanged.connect(self.clear_jiggler)
        self.select_button = button(row, 'Switch to dongle', self.switch_dongle); self.select_button.setEnabled(False)
        button(row, 'Open pairing…', self.pair)
        self.unpair_button = button(row, 'Unpair…', self.unpair); self.unpair_button.setEnabled(False)
        row.addStretch()
        self.jiggler_status = QLabel('Choose a dongle and read its mouse jiggler state.'); self.jiggler_status.setWordWrap(True)
        box.addWidget(self.jiggler_status)
        row = QHBoxLayout(); box.addLayout(row)
        self.read_jiggler = button(row, 'Read jiggler', self.refresh_jiggler)
        self.toggle = button(row, 'Turn on jiggler', self.toggle_jiggler)
        self.mode = QComboBox(); self.mode.addItems(JIGGLER_MODES); row.addWidget(self.mode)
        self.mode.activated.connect(lambda mode: self.set_jiggler('device-jiggler-mode', mode))
        row.addStretch(); box.addStretch()
        self.clear_jiggler(); self.jiggler_timer = QTimer(self); self.jiggler_timer.setSingleShot(True)
        self.jiggler_timer.timeout.connect(self.refresh_jiggler)
        self.addTab(dongles, 'Dongles')

        usb = QWidget(); box = QVBoxLayout(usb)
        self.usb_status = QLabel('Refresh to see the keyboards and mice attached to the controller.'); self.usb_status.setWordWrap(True)
        box.addWidget(self.usb_status)
        row = QHBoxLayout(); box.addLayout(row)
        button(row, 'Refresh USB inputs', self.refresh_usb)
        self.retry = button(row, 'Retry descriptors', lambda: self.change_usb(2)); self.retry.setEnabled(False)
        self.policy = QComboBox(); self.policy.addItem('Automatic', 1); self.policy.addItem('Manual', 0)
        self.policy.setEnabled(False); self.policy.activated.connect(self.change_policy)
        row.addWidget(QLabel('Input selection:')); row.addWidget(self.policy); row.addStretch()
        self.usb_table = QTableWidget(0, 6)
        self.usb_table.setHorizontalHeaderLabels(['Device', 'Interface', 'Functions', 'Channels', 'State', 'Allow device'])
        self.usb_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.usb_table.horizontalHeader().setStretchLastSection(True)
        box.addWidget(self.usb_table)
        self.addTab(usb, 'USB inputs')

    def run(self, work, done, message, status=None):
        status = status or self.route_status
        status.setText(message); self.setEnabled(False)

        def finish(result):
            self.setEnabled(True); done(result)

        def fail(error):
            self.setEnabled(True)
            if status is self.usb_status:
                self.inventory = None; self.usb_table.setRowCount(0)
                self.policy.setEnabled(False); self.retry.setEnabled(False)
                status.setText(f'{error}. Refresh USB inputs before trying again.')
            else: status.setText(str(error))

        self.background(work, finish, fail)

    def command(self, name, done, message, status=None, **values):
        self.run(lambda call: call(name, **values), done, message, status)

    def refresh_dongles(self):
        self.command('device-status', self.show_dongles, 'Reading paired dongles…')

    def show_dongles(self, result):
        self.paired = set(result['paired'])
        for index in range(self.slot.count()):
            slot = self.slot.itemData(index)
            self.slot.setItemText(index, f'Dongle {slot}' + (' — paired' if slot in self.paired else ' — empty'))
        self.unpair_button.setEnabled(self.slot.currentData() in self.paired)
        self.select_button.setEnabled(self.slot.currentData() in self.paired)
        text = f"Selected dongle: {result['selected']}." if result['selected'] else 'No dongle is currently selected.'
        if result['pairing']: text += ' Pairing is open. Hold the new dongle’s button for about 3 seconds.'
        self.route_status.setText(text)

    def confirm(self, title, message):
        return QMessageBox.question(self, title, message, defaultButton=QMessageBox.StandardButton.No) == QMessageBox.StandardButton.Yes

    def pair(self):
        slot = self.slot.currentData()
        message = ('Input pauses during the pairing window. Hold the new dongle’s button for about 3 seconds, then refresh. '
                   'Pairing a replacement will change any existing assignment for this slot.')
        if self.confirm(f'Open pairing for dongle {slot}?', message):
            self.command('device-pair', self.show_dongles, 'Opening pairing…', slot=slot)

    def unpair(self):
        slot = self.slot.currentData()
        if self.confirm(f'Unpair dongle {slot}?', 'This removes the saved pairing from the controller. '
                        'If this is the attached dongle, the companion will lose its connection until it is paired again.'):
            def done(result):
                self.clear_jiggler(); self.show_dongles(result)
                self.route_status.setText(f'Unpair requested for dongle {slot}. Refresh using a paired dongle.')
            self.command('device-unpair', done, 'Removing pairing…', slot=slot)

    def route(self, pause):
        if pause and not self.confirm('Pause input?', 'This stops keyboard and mouse routing. Keep a local input device available to choose Resume input.'):
            return
        self.command('device-pause' if pause else 'device-resume', self.show_dongles, 'Changing input routing…')

    def switch_dongle(self):
        slot = self.slot.currentData()
        def change(call):
            call('select', slot=slot)
            return call('device-status')
        self.run(change, self.show_dongles, 'Switching input…')

    def clear_jiggler(self):
        self.jiggler = None; self.mode.setEnabled(False); self.toggle.setEnabled(False)
        self.unpair_button.setEnabled(self.slot.currentData() in self.paired)
        self.select_button.setEnabled(self.slot.currentData() in self.paired)
        self.jiggler_status.setText('Read the selected dongle’s mouse jiggler state.')
        if hasattr(self, 'jiggler_timer'): self.jiggler_timer.stop()

    def refresh_jiggler(self):
        if self.isEnabled(): self.set_jiggler('device-jiggler-get', 0)

    def set_jiggler(self, command, value):
        if command != 'device-jiggler-get': self.jiggler_attempts = 10
        elif not self.jiggler or not self.jiggler['pending']: self.jiggler_attempts = 0
        self.command(command, self.show_jiggler, 'Reading mouse jiggler…', self.jiggler_status,
                     slot=self.slot.currentData(), value=value)

    def toggle_jiggler(self):
        if self.jiggler: self.set_jiggler('device-jiggler-set', int(not self.jiggler['enabled']))

    def show_jiggler(self, result):
        self.jiggler = result
        self.mode.setCurrentIndex(result['mode']); self.mode.setEnabled(result['supported'] and not result['pending'])
        self.toggle.setEnabled(not result['pending'])
        self.toggle.setText('Turn off jiggler' if result['enabled'] else 'Turn on jiggler')
        text = f"{result['seconds']} seconds idle / interval · {result['distance']} counts"
        if result['pending'] and self.jiggler_attempts:
            self.jiggler_attempts -= 1; self.jiggler_timer.start(500)
        self.jiggler_status.setText(text + (' · waiting for dongle confirmation' if result['pending'] else ' · confirmed'))

    def refresh_usb(self):
        self.run(usb_inventory, self.show_usb, 'Reading USB inputs…', self.usb_status)

    def change_policy(self):
        if self.confirm('Change USB input selection?', 'Changing modes clears saved device overrides. Manual mode keeps the currently active devices.'):
            self.change_usb(0, enabled=self.policy.currentData())
        else: self.policy.setCurrentIndex(self.policy.findData(int(self.inventory['automatic'])))

    def change_usb(self, action, **values):
        if not self.inventory: return
        token = self.inventory['token']
        def work(call):
            call('device-usb-change', token=token, action=action, **values)
            return usb_inventory(call)
        self.run(work, self.show_usb, 'Applying USB input choice…', self.usb_status)

    def show_usb(self, result):
        self.inventory = result
        self.policy.setCurrentIndex(self.policy.findData(int(result['automatic'])))
        self.policy.setEnabled(True); self.retry.setEnabled(True)
        used = result['controls'] + result['hubs'] + result['inputs']
        self.usb_status.setText(f"{used}/{result['limit']} channels · {result['controls']} device controls + "
                                f"{result['hubs']} hub status + {result['inputs']} input endpoints")
        devices = {item['address']: item for item in result['devices']}
        rows = [item for item in result['interfaces'] if item['device_class'] == 3]
        self.usb_table.setRowCount(len(rows)); controlled = set()
        for row, item in enumerate(rows):
            address, number = item['address'], item['number']; device = devices[address]
            functions = ' + '.join(name for i, name in enumerate(('Keyboard', 'Mouse', 'Media', 'System', 'Lock LEDs')) if item['kinds'] & (1 << i))
            columns = (device['name'] or f"{device['vid']}:{device['pid']}", f'{address}/{number}', functions or 'Identifying HID',
                       str(item['endpoints']), f"{item['state']} · {item['decoded']}/{item['reports']} decoded")
            for col, text in enumerate(columns): self.usb_table.setItem(row, col, QTableWidgetItem(text))
            self.usb_table.removeCellWidget(row, 5); self.usb_table.setItem(row, 5, QTableWidgetItem(''))
            if address not in controlled and item['state'] not in ('unsupported', 'removing'):
                controlled.add(address)
                check = QCheckBox(); check.setChecked(item['wanted']); check.setAccessibleName(f'Allow device at USB address {address}')
                check.clicked.connect(lambda checked, a=address, n=number: self.change_usb(1, address=a, number=n, enabled=int(checked)))
                self.usb_table.setCellWidget(row, 5, check)
        self.usb_table.resizeColumnsToContents()
