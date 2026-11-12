"""Small local dashboard; hardware work never runs on Tk's UI thread."""
import copy
from pathlib import Path
import queue
import platform
import sys
import threading
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk
import webbrowser

from . import __version__
from .discovery import discover_config
from .model import Config, Device, PRESETS
from .runner import Runner, demo


class Dashboard:
    def __init__(self, root, profile):
        self.root, self.profile = root, Path(profile).expanduser()
        self.config = Config.load(self.profile) if self.profile.exists() else Config()
        self.messages, self.cancel = queue.Queue(), threading.Event()
        self.worker = None
        self.report = None
        root.title(f"KMSwitcher hardware test {__version__}")
        root.geometry("1100x760")
        root.minsize(800, 580)
        toolbar = ttk.Frame(root, padding=10); toolbar.pack(fill="x")
        for label, command in (("Discover", self.discover), ("Load profile", self.load), ("Save profile", self.save),
                               ("Start test", self.start), ("Stop", self.cancel.set), ("Demo", self.demo), ("Open report", self.open_report)):
            ttk.Button(toolbar, text=label, command=command).pack(side="left", padx=3)
        self.status = tk.StringVar(value="Connect devices, discover, review assignments, then start. No test runs automatically.")
        ttk.Label(root, textvariable=self.status, padding=(12, 4), wraplength=1040).pack(fill="x")
        tabs = ttk.Notebook(root); tabs.pack(fill="both", expand=True, padx=12, pady=8)
        devices = ttk.Frame(tabs, padding=8); settings = ttk.Frame(tabs, padding=12); output = ttk.Frame(tabs, padding=8)
        tabs.add(devices, text="Devices"); tabs.add(settings, text="Test setup"); tabs.add(output, text="Progress / results")
        ttk.Label(devices, text="Ctrl-click or Shift-click to select multiple devices; Ctrl+A selects all.\n"
                  "Enable only the slaves participating in this test. Identities are checked before every run.").pack(anchor="w", pady=5)
        columns = ("role", "enabled", "name", "identity", "uart", "slot")
        self.devices = ttk.Treeview(devices, columns=columns, show="headings", selectmode="extended")
        for key, width in zip(columns, (95, 65, 130, 135, 400, 50)):
            self.devices.heading(key, text=key.title()); self.devices.column(key, width=width, minwidth=40)
        self.devices.pack(fill="both", expand=True)
        self.devices.bind("<Double-1>", lambda _: self.edit())
        self.devices.bind("<Control-a>", self.select_all)
        self.devices.bind("<<TreeviewSelect>>", lambda _: self.update_selection_actions())
        buttons = ttk.Frame(devices); buttons.pack(fill="x", pady=8)
        self.single_selection_buttons = []
        for label, command in (("Edit selected", self.edit), ("Enable selected", lambda: self.set_enabled(True)),
                               ("Disable selected", lambda: self.set_enabled(False)), ("Add device", self.add), ("Remove selected", self.remove)):
            button = ttk.Button(buttons, text=label, command=command)
            button.pack(side="left", padx=4)
            if label in ("Edit selected", "Remove selected"): self.single_selection_buttons.append(button)
        self.fields = {}
        labels = (("preset", "Test preset"), ("rates", "Reports/s per generator (comma-separated)"),
                  ("seconds", "Seconds per segment"), ("soak_seconds", "Soak / All tests total, or Mixed soak per receiver (seconds)"),
                  ("seed", "Random seed (0–4294967295)"), ("max_log_mb", "Capture / trace budget (MB)"), ("output", "Results directory"),
                  ("campaign_repeats", "Campaign attempts per failing case"), ("campaign_idle_seconds", "Campaign / Mixed soak idle (seconds)"),
                  ("fixture_notes", "Fixture / hub notes"), ("recovery_attempts", "Recovery attempts: All tests"))
        for row, (key, label) in enumerate(labels):
            ttk.Label(settings, text=label).grid(row=row, column=0, sticky="w", pady=8, padx=4)
            value = getattr(self.config, key)
            self.fields[key] = tk.StringVar(value=", ".join(map(str, value)) if key == "rates" else str(value))
            widget = (ttk.Combobox(settings, textvariable=self.fields[key], values=PRESETS, state="readonly") if key == "preset"
                      else ttk.Entry(settings, textvariable=self.fields[key]))
            widget.grid(row=row, column=1, sticky="ew", padx=8)
        settings.columnconfigure(1, weight=1)
        self.stop_on_failure = tk.BooleanVar(value=self.config.stop_on_failure)
        ttk.Checkbutton(settings, text="Stop after first failing case (including campaign repeats)", variable=self.stop_on_failure).grid(row=len(labels), column=0, columnspan=2, sticky="w", pady=10)
        ttk.Label(settings, text="The master stays paused after a test. Physical power cycles need manual action or a controllable hub.\n"
                  "Routing coverage and evidence limitations are listed in each report.", wraplength=950).grid(row=len(labels) + 1, column=0, columnspan=2, sticky="w", pady=12)
        self.workload = tk.StringVar()
        ttk.Label(settings, textvariable=self.workload, wraplength=950).grid(row=len(labels) + 2, column=0, columnspan=2, sticky="w")
        ttk.Button(settings, text="Replay USB recipe…", command=self.import_campaign_recipe).grid(row=len(labels) + 3, column=0, sticky="w")
        ttk.Button(settings, text="Clear campaign replay", command=self.clear_campaign_recipe).grid(row=len(labels) + 3, column=1, sticky="w")
        for variable in [*self.fields.values(), self.stop_on_failure]:
            variable.trace_add("write", lambda *_: self.update_workload())
        ttk.Button(output, text="Copy diagnostics", command=self.copy_diagnostics).pack(side="bottom", anchor="e", pady=6)
        self.output = tk.Text(output, wrap="word", state="disabled", font=("TkFixedFont", 10))
        scrollbar = ttk.Scrollbar(output, command=self.output.yview)
        self.output.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y"); self.output.pack(fill="both", expand=True)
        self.refresh()
        root.report_callback_exception = self.callback_error
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(100, self.poll)

    def busy(self): return self.worker is not None and self.worker.is_alive()

    def guard_idle(self):
        if self.busy():
            messagebox.showinfo("Test in progress", "Stop the current operation before editing or starting another.")
            return False
        return True

    def refresh(self):
        self.devices.delete(*self.devices.get_children())
        self.rows = [("master", self.config.master)] + [("generator", d) for d in self.config.generators] + [("receiver", d) for d in self.config.receivers]
        for index, (role, d) in enumerate(self.rows):
            self.devices.insert("", "end", iid=str(index), values=(role, "yes" if d.enabled else "no", d.name, d.identity, d.uart, d.slot or ""))
        self.update_selection_actions()
        self.update_workload()

    def update_workload(self):
        try: self.workload.set(self.settings().workload_text())
        except ValueError: self.workload.set("Enter valid settings to preview the workload.")

    def selected(self):
        items = self.devices.selection()
        return self.rows[int(items[0])] if len(items) == 1 else None

    def select_all(self, event=None):
        self.devices.selection_set(self.devices.get_children())
        return "break"

    def update_selection_actions(self):
        state = "normal" if len(self.devices.selection()) == 1 else "disabled"
        for button in self.single_selection_buttons: button.configure(state=state)

    def settings(self):
        config = copy.deepcopy(self.config)
        for key, variable in self.fields.items():
            value = variable.get().strip()
            setattr(config, key, ([int(v.strip()) for v in value.split(",") if v.strip()] if key == "rates" else
                                  int(value) if key in ("seconds", "soak_seconds", "seed", "max_log_mb", "campaign_repeats", "campaign_idle_seconds", "recovery_attempts") else value))
        config.stop_on_failure = self.stop_on_failure.get()
        return config

    def import_campaign_recipe(self):
        if not self.guard_idle(): return
        path = filedialog.askopenfilename(filetypes=[("USB campaign recipe", "*.json")])
        if not path: return
        try:
            if Path(path).stat().st_size > 16384: raise ValueError("Campaign recipe is too large")
            candidate = self.settings()
            candidate.campaign_recipe = Path(path).read_text(encoding="utf-8")
            candidate.preset = "usb-stall"; candidate.validate_campaign()
            self.config.campaign_recipe = candidate.campaign_recipe
            self.fields["preset"].set("usb-stall"); self.update_workload()
        except (ValueError, OSError, TypeError, KeyError) as error: messagebox.showerror("Recipe unavailable", str(error))

    def clear_campaign_recipe(self):
        if not self.guard_idle(): return
        self.config.campaign_recipe = ""; self.update_workload()

    def save(self):
        if not self.guard_idle(): return
        try:
            self.config = self.settings(); self.config.save(self.profile)
            self.status.set(f"Saved {self.profile}")
        except Exception as error: messagebox.showerror("Profile", str(error))

    def load(self):
        if not self.guard_idle(): return
        path = filedialog.askopenfilename(filetypes=[("JSON profile", "*.json")])
        if not path: return
        try:
            self.config, self.profile = Config.load(path), Path(path)
            for key, variable in self.fields.items():
                value = getattr(self.config, key); variable.set(", ".join(map(str, value)) if key == "rates" else str(value))
            self.stop_on_failure.set(self.config.stop_on_failure); self.refresh()
        except Exception as error: messagebox.showerror("Profile", str(error))

    def edit(self):
        if not self.guard_idle() or not self.selected(): return
        role, device = self.selected()
        dialog = tk.Toplevel(self.root); dialog.title(f"Edit {role}"); dialog.transient(self.root); dialog.grab_set()
        variables = {}
        keys = ("name", "identity", "uart") if role != "receiver" else ("name", "identity", "slot")
        for row, key in enumerate(keys):
            ttk.Label(dialog, text=key.title()).grid(row=row, column=0, padx=10, pady=8, sticky="w")
            variables[key] = tk.StringVar(value=str(getattr(device, key)))
            ttk.Entry(dialog, textvariable=variables[key], width=65).grid(row=row, column=1, padx=10, pady=8)
        def apply():
            try:
                changes = {key: int(v.get()) if key == "slot" else v.get().strip() for key, v in variables.items()}
                for key, value in changes.items(): setattr(device, key, value)
                self.refresh(); dialog.destroy()
            except ValueError as error: messagebox.showerror("Device", str(error), parent=dialog)
        ttk.Button(dialog, text="Apply", command=apply).grid(row=len(keys), column=1, sticky="e", padx=10, pady=10)

    def set_enabled(self, enabled):
        if not self.guard_idle(): return
        for item in self.devices.selection():
            role, device = self.rows[int(item)]
            if role != "master":
                device.enabled = enabled
                self.devices.set(item, "enabled", "yes" if enabled else "no")

    def add(self):
        if not self.guard_idle(): return
        role = simpledialog.askstring("Add device", "Role: generator or receiver", parent=self.root)
        if role not in ("generator", "receiver"): return
        collection = self.config.generators if role == "generator" else self.config.receivers
        collection.append(Device("", f"New {role}", enabled=False)); self.refresh()
        self.devices.selection_set(str(len(self.rows) - 1) if role == "receiver" else str(len(self.config.generators)))
        self.edit()

    def remove(self):
        if self.guard_idle() and self.selected():
            role, device = self.selected()
            if role == "generator": self.config.generators.remove(device)
            elif role == "receiver": self.config.receivers.remove(device)
            self.refresh()

    def launch(self, work):
        if not self.guard_idle(): return
        self.cancel.clear()
        def run():
            try: work()
            except Exception: self.messages.put(("error", traceback.format_exc()))
        self.worker = threading.Thread(target=run, daemon=True); self.worker.start()

    def discover(self):
        def work():
            config = discover_config(self.config, lambda text: self.messages.put(("text", text)), self.cancel)
            self.messages.put(("discovered", config))
        self.launch(work)

    def start(self):
        if not self.guard_idle(): return
        try:
            config = self.settings(); config.validate(); config.save(self.profile); self.config = config
        except Exception as error: messagebox.showerror("Test setup", str(error)); return
        def work():
            _, path = Runner(config, lambda text: self.messages.put(("text", text)), self.cancel).run()
            self.messages.put(("report", path / "report.html"))
        self.launch(work)

    def demo(self):
        try: config = self.settings()
        except Exception as error: messagebox.showerror("Test setup", str(error)); return
        def work():
            result, path = demo(config)
            self.messages.put(("text", result["status"] + ": simulated devices only"))
            self.messages.put(("report", path / "report.html"))
        self.launch(work)

    def poll(self):
        # Schedule the next poll even when a Tk callback raises. Previously one
        # error stopped the queue permanently, leaving only a terminal traceback.
        try: self.process_messages()
        finally: self.root.after(100, self.poll)

    def process_messages(self):
        while not self.messages.empty():
            kind, value = self.messages.get_nowait()
            if kind == "discovered":
                self.config = value
                self.refresh()
            elif kind == "report": self.report = Path(value); self.status.set(f"Report saved: {self.report}")
            else:
                self.status.set(value)
                self.output.configure(state="normal"); self.output.insert("end", value + "\n"); self.output.see("end")
                if int(self.output.index("end-1c").split(".")[0]) > 2000: self.output.delete("1.0", "501.0")
                self.output.configure(state="disabled")
                if kind == "error": messagebox.showerror("Operation failed", value.splitlines()[-1])

    def copy_diagnostics(self):
        details = (f"KMSwitcher hardware test {__version__}\n{platform.platform()}\n"
                   f"Python {sys.version}\nProfile: {self.profile}\n\n" + self.output.get("1.0", "end-1c"))
        self.root.clipboard_clear()
        self.root.clipboard_append(details)
        self.status.set("Diagnostics copied. Paste them into the support chat.")

    def callback_error(self, error_type, error, tb):
        detail = "".join(traceback.format_exception(error_type, error, tb))
        self.output.configure(state="normal")
        self.output.insert("end", "UI callback failed:\n" + detail + "\n")
        self.output.see("end")
        self.output.configure(state="disabled")
        self.status.set("UI error recorded in Progress / results. Use Copy diagnostics to share it.")

    def open_report(self):
        if self.report: webbrowser.open(self.report.resolve().as_uri())

    def close(self):
        if self.busy():
            self.cancel.set(); self.status.set("Stopping generators and closing capture before exiting…")
            self.root.after(100, self.close)
        else: self.root.destroy()


def show(profile):
    root = tk.Tk(); Dashboard(root, profile); root.mainloop()
