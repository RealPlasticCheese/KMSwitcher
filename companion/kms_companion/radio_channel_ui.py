"""Manual channel selection and automatic recovery settings."""
from .radio_channel import MAX_COOLDOWN, MIN_COOLDOWN, RECOVERY_PHASES, RECOVERY_RESULTS


def create_dialog(parent, submit):
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QFormLayout, QGroupBox,
                                   QHBoxLayout, QLabel, QPushButton, QSpinBox, QVBoxLayout)

    class RadioDialog(QDialog):
        def __init__(self):
            super().__init__(parent)
            self.setWindowTitle("Radio channel")
            self.setMinimumWidth(440)
            self.state = None
            self.recovery_state = None
            self.refresh_recovery = False
            self.recovery_busy = False
            layout = QVBoxLayout(self)
            explanation = QLabel("All paired dongles follow the master's channel. Manual changes pause input. "
                                 "Wait for dongles to reconnect, then choose Resume routing.")
            explanation.setWordWrap(True)
            layout.addWidget(explanation)
            self.status = QLabel("Load radio status to begin.")
            self.status.setWordWrap(True)
            layout.addWidget(self.status)
            self.channels = QComboBox()
            layout.addWidget(self.channels)
            row = QHBoxLayout()
            self.refresh = QPushButton("Refresh")
            self.save = QPushButton("Save channel")
            self.resume = QPushButton("Resume routing")
            for button in (self.refresh, self.save, self.resume):
                row.addWidget(button)
            layout.addLayout(row)
            group = QGroupBox("Automatic channel recovery")
            settings = QFormLayout(group)
            help_text = QLabel("Trials use measured packet delivery and pause input while the master checks "
                               "the new channel. Routing resumes when the selected dongle is ready.")
            help_text.setWordWrap(True)
            settings.addRow(help_text)
            self.recovery_enabled = QCheckBox("Try a different channel when delivery stays poor")
            self.single_peer = QCheckBox("Allow a trial with one reachable dongle")
            self.single_peer.setToolTip("With one reachable dongle, a trial requires a longer period of radio delivery faults.")
            self.cooldown = QSpinBox()
            self.cooldown.setRange(MIN_COOLDOWN, MAX_COOLDOWN)
            self.cooldown.setSuffix(" min")
            settings.addRow(self.recovery_enabled)
            settings.addRow(self.single_peer)
            settings.addRow("Time between trials", self.cooldown)
            self.recovery_save = QPushButton("Save recovery settings")
            settings.addRow(self.recovery_save)
            self.recovery_status = QLabel("Load radio status to read the master's recovery settings.")
            self.recovery_status.setWordWrap(True)
            settings.addRow(self.recovery_status)
            layout.addWidget(group)
            layout.addStretch()
            self.refresh.clicked.connect(self.load)
            self.save.clicked.connect(self.change)
            self.resume.clicked.connect(lambda: self.request("channel-resume"))
            self.save.setEnabled(False)
            self.resume.setEnabled(False)
            self.set_recovery_controls(False)
            self.recovery_save.clicked.connect(self.save_recovery)
            self.timer = QTimer(self)
            self.timer.setSingleShot(True)
            self.timer.timeout.connect(self.refresh_after_switch)

        def request(self, command, **args):
            for button in (self.refresh, self.save, self.resume):
                button.setEnabled(False)
            self.status.setText("Working…")
            submit(command, callback=lambda state: self.show_state(state, command), **args)

        def load(self):
            self.refresh_recovery = True
            self.request("channel-get")

        def refresh_after_switch(self):
            if self.recovery_state and any(value != self.recovery_state[key]
                                           for key, value in self.recovery_options().items()):
                self.request("channel-get")
            else:
                self.load()

        def change(self):
            channel = self.channels.currentData()
            if self.state and channel != self.state['current']:
                self.request("channel-set", channel=channel)

        def show_state(self, state, command="channel-get"):
            self.state = state
            self.channels.clear()
            for channel in range(state['first'], state['first'] + state['count']):
                self.channels.addItem(str(channel), channel)
            self.channels.setCurrentIndex(self.channels.findData(state['pending']))
            changing = state['current'] != state['pending']
            supported = state['supported'] == state['paired']
            message = (f"Current channel: {state['current']}. "
                       f"Updated dongles: {state['supported']}/{state['paired']}.")
            if changing:
                message += f" Switching to {state['pending']}; routing remains paused."
                self.timer.start(2500)
            elif command == 'channel-resume':
                message += " Routing resumed."
            else:
                message += " After a change, wait for dongles to reconnect before resuming."
            if not supported:
                message += " Update all paired dongles before changing channels."
            self.status.setText(message)
            self.refresh.setEnabled(True)
            self.update_channel_controls()
            if self.refresh_recovery:
                self.refresh_recovery = False
                self.request_recovery("channel-recovery-get")

        def set_recovery_controls(self, enabled):
            for control in (self.recovery_enabled, self.single_peer, self.cooldown, self.recovery_save):
                control.setEnabled(enabled)

        def update_channel_controls(self):
            if not self.state:
                return
            changing = self.state['current'] != self.state['pending']
            trial = self.recovery_state and self.recovery_state['phase']
            self.save.setEnabled(self.state['supported'] == self.state['paired'] and (not changing or bool(trial)))
            self.resume.setEnabled(not changing and not trial)

        def request_recovery(self, command, **args):
            if self.recovery_busy:
                return
            self.recovery_busy = True
            self.set_recovery_controls(False)
            self.recovery_status.setText("Reading recovery settings…" if not args else "Saving recovery settings…")
            submit(command, callback=self.show_recovery_state, **args)

        def recovery_options(self):
            return dict(enabled=self.recovery_enabled.isChecked(), single_peer=self.single_peer.isChecked(),
                        cooldown=self.cooldown.value())

        def save_recovery(self):
            if self.recovery_state:
                self.request_recovery("channel-recovery-set", **self.recovery_options())

        def show_recovery_state(self, state):
            self.recovery_busy = False
            self.recovery_state = state
            self.recovery_enabled.setChecked(state['enabled'])
            self.single_peer.setChecked(state['single_peer'])
            self.cooldown.setValue(state['cooldown'])
            message = RECOVERY_PHASES[state['phase']] if state['enabled'] or state['phase'] else "Off"
            message += f". Trials used in the current budget window: {state['attempts']}/{state['limit']}."
            if state['result']:
                message += f" {RECOVERY_RESULTS[state['result']]}."
            if state['candidate']:
                message += f" Channel {state['old']} → {state['candidate']}."
            self.recovery_status.setText(message)
            self.set_recovery_controls(True)
            self.update_channel_controls()
            if state['phase'] and not self.timer.isActive():
                self.timer.start(2500)

        def show_error(self, error, command="channel-get"):
            if command.startswith("channel-recovery-"):
                self.recovery_busy = False
                self.recovery_status.setText(str(error))
                self.set_recovery_controls(False)
                return
            self.refresh_recovery = False
            self.status.setText(str(error))
            self.refresh.setEnabled(True)
            self.save.setEnabled(False)
            self.resume.setEnabled(False)

    return RadioDialog()
