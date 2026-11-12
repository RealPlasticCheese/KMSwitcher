"""Quick reference for shortcuts handled by the controller firmware."""
from PySide6.QtWidgets import QLabel, QTextBrowser, QVBoxLayout, QWidget

from .preferences import TRIGGERS


class ShortcutsPanel(QWidget):
    def __init__(self):
        super().__init__()
        layout = QVBoxLayout(self)
        self.trigger = QLabel()
        self.trigger.setWordWrap(True)
        layout.addWidget(self.trigger)
        guide = QTextBrowser()
        guide.setHtml("""
            <h2>Controller shortcuts</h2>
            <p>Tap and release the same trigger key twice on the same keyboard,
            then press the keys below in order. Use the keyboard connected to the
            KMSwitcher controller. Top-row and keypad digits both work.</p>
            <p><b>1–7 · Switch dongle</b><br>
            Select the paired dongle for that slot.</p>
            <p><b>P, then 1–7 · Add a dongle</b><br>
            Choose a slot. While the receiver is running, hold its BOOT button
            for 3 seconds and release within the 30-second pairing window. After pairing,
            double-tap the trigger and press the slot number to switch to it.</p>
            <p><b>D, then 1–7 · Set the startup dongle</b><br>
            Save the slot to select on the next controller startup.</p>
            <p><b>J · Toggle the mouse jiggler</b><br>
            Turn the selected dongle’s jiggler on or off.</p>
            <p><b>K · Cycle the mouse jiggler mode</b><br>
            Cycle through Left/right, Four-way, and Random on the selected dongle.
            The jiggler keeps its current on/off state.</p>
            <p><b>C, then the new trigger · Change the trigger key</b><br>
            Release C, then press and release Ctrl, Alt, Shift, Scroll Lock,
            Insert, Pause/Break, or Num Lock on the same keyboard within 10 seconds.</p>
            <p><b>Esc · Cancel a shortcut</b><br>
            Cancel after the double-tap or while choosing a slot or new trigger.</p>
            <h2>Diagnostics in a plain-text editor</h2>
            <p>On the selected computer, focus an empty document in Notepad or
            another plain-text editor. Use a US keyboard layout.</p>
            <p><b>Double-tap the trigger, then L · Type diagnostic logs</b><br>
            Release the keys to let the controller type the log snapshot.
            A working selected dongle and captured logs are required.
            Press Esc to stop the dump.</p>
            <p><b>I while logs are printing · Open Keyboard Console</b><br>
            Type <code>help</code> and press Enter to list commands. Use Backspace
            to edit a command and Enter to run it. Esc stops output while it’s
            printing. When the console is idle, Esc exits and restores normal typing.</p>
        """)
        layout.addWidget(guide)
        self.show_unavailable()

    def show_preferences(self, doc):
        values = doc["values"]
        self.trigger.setText(f"Trigger key: {TRIGGERS[values['trigger']]}  ·  "
                             f"Double-tap window: {values['double_ms']:,} ms  ·  "
                             f"Command window: {values['select_ms']:,} ms")

    def show_unavailable(self):
        self.trigger.setText("Saved trigger unavailable. Use your configured trigger (Ctrl by default).")
