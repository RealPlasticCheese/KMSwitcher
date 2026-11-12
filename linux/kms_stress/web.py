"""Local browser dashboard using the same hardware engine as the Tk dashboard."""
from collections import deque
import copy
from dataclasses import asdict
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import tempfile
import threading
import traceback
from urllib.parse import quote, unquote, urlsplit
import zipfile

from . import __version__
from .discovery import discover_config
from .model import Config, Device, PRESETS
from .runner import Runner, demo
from .restarts import RestartCancelled, restart_devices, restart_targets


class BusyError(RuntimeError):
    pass


def parse_config(data):
    """Check JSON shape even when saving an unfinished fixture configuration."""
    if not isinstance(data, dict):
        raise ValueError("Profile must be a JSON object")
    data = dict(data)
    if data.pop("version", 1) != 1:
        raise ValueError("Unsupported profile version")
    try:
        data["master"] = Device(**data["master"])
        for key in ("generators", "receivers", "masters"):
            if key == "masters" and data.get(key) is None:
                continue
            if not isinstance(data[key], list) or len(data[key]) > 32:
                raise ValueError("Device lists must contain at most 32 entries")
            data[key] = [Device(**item) for item in data[key]]
        config = Config(**data)
    except (KeyError, TypeError) as error:
        raise ValueError(f"Invalid profile: {error}") from error
    for device in [config.master, *(config.masters or []), *config.generators, *config.receivers]:
        if (not all(isinstance(getattr(device, key), str) for key in ("name", "identity", "uart"))
                or type(device.enabled) is not bool or type(device.slot) is not int):
            raise ValueError("Invalid device field types")
    if config.preset not in PRESETS:
        raise ValueError("Select a known test preset")
    if not isinstance(config.rates, list) or not config.rates or any(type(rate) is not int or not 1 <= rate <= 1000 for rate in config.rates):
        raise ValueError("Report rates must be integers between 1 and 1000")
    for key, minimum, maximum in (("seconds", 1, 3600), ("soak_seconds", 1, 604800),
                                  ("seed", 0, 0xffffffff), ("max_log_mb", 16, 65536)):
        value = getattr(config, key)
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"{key} must be an integer between {minimum} and {maximum}")
    if type(config.stop_on_failure) is not bool or not isinstance(config.output, str) or not config.output.strip():
        raise ValueError("Choose an output directory and a boolean stop-on-failure setting")
    if type(config.restart_before_run) is not bool:
        raise ValueError("Restart-before-run must be boolean")
    config.validate_campaign()
    try:
        Path(config.output).expanduser().resolve()
    except (ValueError, OSError, RuntimeError) as error:
        raise ValueError(f"Invalid output directory: {error}") from error
    return config


class Controller:
    """Serialize hardware operations and retain bounded progress across reconnects."""
    def __init__(self, profile, config):
        self.profile, self.config = Path(profile).expanduser(), parse_config(asdict(config))
        self.lock = threading.RLock()
        self.cancel = threading.Event()
        self.worker = None
        self.closing = False
        self.operation = "idle"
        self.revision = 0
        self.messages = deque(maxlen=2000)
        self.report = None
        self.runner = None
        self.notify("Connect devices, discover, review assignments, then start. No test runs automatically.")

    def notify(self, message):
        with self.lock:
            stamp = datetime.now().astimezone().isoformat(sep=" ", timespec="seconds")
            lines = str(message).splitlines() or [""]
            self.messages.append("\n".join(f"[{stamp}] {line}" for line in lines))

    def guard_idle(self):
        if self.closing or (self.worker and self.worker.is_alive()):
            raise BusyError("Stop the current operation and wait for cleanup before making changes.")

    def snapshot(self):
        with self.lock:
            return {"version": __version__, "profile": str(self.profile), "presets": PRESETS,
                    "config": asdict(self.config), "revision": self.revision,
                    "busy": bool(self.worker and self.worker.is_alive()), "operation": self.operation,
                    "messages": list(self.messages), "report": self.report,
                    "progress": self.runner.progress() if self.runner else None,
                    "workload": self.config.workload(), "workload_text": self.config.workload_text()}

    def configure(self, data, save=False):
        config = parse_config(data)
        with self.lock:
            self.guard_idle()
            if save:
                config.save(self.profile)
                self.notify(f"Saved {self.profile}")
            if config.output != self.config.output:
                self.report = None
            self.config = config
            self.revision += 1

    def launch(self, operation, data, target=None):
        config = parse_config(data)
        if operation == "run":
            config.validate()
        elif operation == "restart":
            restart_targets(config, target)
        with self.lock:
            self.guard_idle()
            if operation == "run":
                config.save(self.profile)
            if operation in ("run", "demo") or config.output != self.config.output:
                self.report = None
            self.config = config
            self.revision += 1
            self.cancel.clear()
            self.operation = operation
            self.runner = None
            self.notify(f"Starting {operation}…")
            self.worker = threading.Thread(target=self._work, args=(operation, copy.deepcopy(config), target), name="hardware-test", daemon=False)
            self.worker.start()

    def _work(self, operation, config, target=None):
        try:
            if operation == "discover":
                discovered = discover_config(config, self.notify, self.cancel)
                with self.lock:
                    self.config = discovered
                    self.revision += 1
            elif operation == "restart":
                results = restart_devices(config, self.notify, self.cancel, target=target)
                self.notify(f"Restarted {len(results)} device(s). Routing remains paused; ready for a new test.")
            else:
                if operation == "demo":
                    result, path = demo(config)
                else:
                    with self.lock: self.runner = Runner(config, self.notify, self.cancel)
                    result, path = self.runner.run()
                with self.lock:
                    self.report = path.name
                self.notify(f"{result['status']}: {result.get('message', '')} Report saved: {path}")
        except RestartCancelled as error:
            self.notify(str(error))
        except Exception:
            if self.runner and self.runner.run_progress:
                self.runner.run_progress.finish("INCONCLUSIVE")
            self.notify(traceback.format_exc())
        finally:
            with self.lock:
                self.operation = "idle"

    def stop(self):
        with self.lock:
            self.cancel.set()
            if self.worker and self.worker.is_alive():
                self.notify("Stop requested; waiting for the operation and hardware cleanup to finish.")

    def close(self):
        with self.lock:
            self.closing = True
            self.cancel.set()
            worker = self.worker
        if worker:
            worker.join()

    def report_store(self):
        with self.lock:
            return ReportStore(self.config.output)


class ReportStore:
    """Only expose completed run directories within the configured results root."""
    def __init__(self, output):
        self.root = Path(output).expanduser().resolve()

    def directory(self, name):
        if not name or name in (".", "..") or "/" in name or "\\" in name:
            raise FileNotFoundError("Unknown report")
        directory = self.root / name
        if directory.is_symlink() or directory.resolve().parent != self.root or not directory.is_dir():
            raise FileNotFoundError("Unknown report")
        for filename in ("report.html", "result.json"):
            path = directory / filename
            if path.is_symlink() or not path.is_file():
                raise FileNotFoundError("Report is not complete")
        return directory

    def list(self):
        reports = []
        if self.root.is_dir():
            for child in sorted(self.root.iterdir(), reverse=True):
                try:
                    directory = self.directory(child.name)
                    with (directory / "result.json").open(encoding="utf-8") as stream:
                        result = json.load(stream)
                    reports.append({"name": child.name, "status": str(result.get("status", "Unknown")),
                                    "message": str(result.get("message", "")),
                                    "modified": (directory / "report.html").stat().st_mtime})
                except (OSError, ValueError, AttributeError):
                    continue
        return {"root": str(self.root), "reports": reports}

    def archive(self, name):
        directory = self.directory(name)
        # Large captures go to a temporary disk file, never a whole-archive RAM buffer.
        stream = tempfile.TemporaryFile()
        try:
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
                for folder, directories, filenames in os.walk(directory, followlinks=False):
                    directories[:] = sorted(d for d in directories if not (Path(folder) / d).is_symlink())
                    for filename in sorted(filenames):
                        path = Path(folder) / filename
                        if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode) or not path.resolve().is_relative_to(directory):
                            continue
                        archive.write(path, (Path(name) / path.relative_to(directory)).as_posix())
            stream.seek(0)
            return stream
        except Exception:
            stream.close()
            raise


class WebServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, controller):
        self.controller = controller
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        # Keep frequent dashboard polling out of the server terminal.
        pass

    def headers_for(self, status, content_type, length=None, policy="default-src 'self'; frame-ancestors 'none'"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", policy)
        if length is not None:
            self.send_header("Content-Length", str(length))

    def send_json(self, value, status=200):
        data = json.dumps(value).encode("utf-8")
        self.headers_for(status, "application/json; charset=utf-8", len(data))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlsplit(self.path)
        try:
            if url.path == "/api/state":
                self.send_json(self.server.controller.snapshot())
            elif url.path == "/api/reports":
                self.send_json(self.server.controller.report_store().list())
            elif url.path.startswith("/reports/"):
                parts = url.path.split("/")
                if len(parts) != 4:
                    raise FileNotFoundError("Unknown report URL")
                name, action = unquote(parts[2]), parts[3]
                store = self.server.controller.report_store()
                if action == "download.zip":
                    with store.archive(name) as stream:
                        length = stream.seek(0, os.SEEK_END)
                        stream.seek(0)
                        self.headers_for(200, "application/zip", length)
                        self.send_header("Content-Disposition", f"attachment; filename*=UTF-8''{quote(name, safe='')}.zip")
                        self.end_headers()
                        shutil.copyfileobj(stream, self.wfile, length=1024 * 1024)
                elif action == "report.html":
                    data = (store.directory(name) / "report.html").read_bytes()
                    self.headers_for(200, "text/html; charset=utf-8", len(data),
                                     "sandbox; default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'")
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    raise FileNotFoundError("Unknown report action")
            elif url.path in ("/", "/app.js", "/style.css"):
                filename, mime = {"/": ("index.html", "text/html"), "/app.js": ("app.js", "text/javascript"),
                                  "/style.css": ("style.css", "text/css")}[url.path]
                data = (Path(__file__).parent / "web_assets" / filename).read_bytes()
                self.headers_for(200, mime + "; charset=utf-8", len(data))
                self.end_headers()
                self.wfile.write(data)
            else:
                raise FileNotFoundError("Unknown URL")
        except (BrokenPipeError, ConnectionResetError):
            pass
        except FileNotFoundError as error:
            self.send_json({"error": str(error)}, 404)
        except OSError as error:
            self.send_json({"error": str(error)}, 500)

    def do_POST(self):
        if self.headers.get("X-KMS-Request") != "1":
            self.send_json({"error": "Expected a dashboard request"}, 403)
            return
        origin = self.headers.get("Origin")
        if origin and origin != "http://" + self.headers.get("Host", ""):
            self.send_json({"error": "Cross-origin requests are not allowed"}, 403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 128 * 1024:
                raise ValueError("Expected a JSON request of at most 128 KiB")
            data = json.loads(self.rfile.read(length))
            controller = self.server.controller
            path = urlsplit(self.path).path
            if path == "/api/preview":
                config = parse_config(data)
                self.send_json({"workload": config.workload(), "text": config.workload_text()})
                return
            elif path == "/api/profile":
                controller.configure(data, save=True)
            elif path in ("/api/discover", "/api/run", "/api/demo"):
                controller.launch(path.rsplit("/", 1)[1], data)
            elif path == "/api/restart":
                if not isinstance(data, dict) or "config" not in data:
                    raise ValueError("Supply the fixture configuration and an optional restart target")
                controller.launch("restart", data["config"], data.get("target"))
            elif path == "/api/stop":
                controller.stop()
            else:
                raise FileNotFoundError("Unknown action")
            self.send_json(controller.snapshot())
        except BusyError as error:
            self.send_json({"error": str(error)}, 409)
        except (ValueError, TypeError, UnicodeError) as error:
            self.send_json({"error": str(error)}, 400)
        except FileNotFoundError as error:
            self.send_json({"error": str(error)}, 404)
        except OSError as error:
            self.send_json({"error": str(error)}, 500)


def serve(profile, config, host="127.0.0.1", port=8765, *, open_browser=False):
    controller = Controller(profile, config)
    server = WebServer((host, port), controller)
    stopped = threading.Event()
    previous = {}

    def stop(*_):
        if not stopped.is_set():
            stopped.set()
            controller.stop()
            threading.Thread(target=server.shutdown, daemon=True).start()

    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, stop)
        address = "127.0.0.1" if host == "0.0.0.0" else host
        print(f"KMSwitcher hardware test {__version__}\nOpen http://{address}:{server.server_port}/", flush=True)
        if host == "0.0.0.0":
            print("For another computer, replace 127.0.0.1 with this Linux machine's LAN address. Access is limited by network reachability.", flush=True)
        print("Ctrl+C stops the server and waits for hardware cleanup. Closing a browser tab leaves tests running.", flush=True)
        if open_browser:
            import webbrowser
            webbrowser.open(f"http://{address}:{server.server_port}/")
        server.serve_forever(poll_interval=0.2)
    finally:
        controller.close()
        server.server_close()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
