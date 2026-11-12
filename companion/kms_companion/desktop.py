"""Platform boundary: coordinates, monitor discovery, readiness and pointer placement."""
import ctypes
import ctypes.util
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from .geometry import Rect


class UnsupportedDesktop(RuntimeError):
    pass


def _system_environment():
    """Run host desktop tools against host libraries, including from a helper."""
    environment = dict(os.environ)
    if not getattr(sys, "frozen", False):
        return environment
    bundle = Path(sys._MEIPASS).resolve()
    # A helper spawned by the frozen GUI can inherit the bundle in ORIG too.
    paths = [path for path in environment.get("LD_LIBRARY_PATH_ORIG", "").split(os.pathsep)
             if path and not Path(path).resolve().is_relative_to(bundle)]
    if paths:
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    else:
        environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("LD_LIBRARY_PATH_ORIG", None)
    return environment


class WindowsDesktop:
    name = "Windows"

    def __init__(self):
        from ctypes import wintypes as w
        self.w = w
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        # Keep cursor and monitor rectangles in the same physical pixel space.
        self.user.SetProcessDpiAwarenessContext.argtypes = [w.HANDLE]
        self.user.SetProcessDpiAwarenessContext(w.HANDLE(-4))
        self.user.GetCursorPos.argtypes = [ctypes.POINTER(w.POINT)]
        self.user.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
        self.user.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.user.OpenInputDesktop.restype = w.HANDLE
        self.user.CloseDesktop.argtypes = [w.HANDLE]
        self.user.GetUserObjectInformationW.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD)]
        self._callback = ctypes.WINFUNCTYPE(w.BOOL, w.HANDLE, w.HDC, ctypes.POINTER(w.RECT), w.LPARAM)
        self.user.EnumDisplayMonitors.argtypes = [w.HDC, ctypes.POINTER(w.RECT), self._callback, w.LPARAM]

    def screens(self):
        rects = []

        @self._callback
        def collect(monitor, dc, rect, data):
            r = rect.contents
            rects.append(Rect(r.left, r.top, r.right - r.left, r.bottom - r.top))
            return True

        if not self.user.EnumDisplayMonitors(None, None, collect, 0):
            raise OSError("Cannot enumerate monitors")
        return sorted(set(rects), key=lambda r: (r.x, r.y, r.width, r.height))

    def pointer(self):
        point = self.w.POINT()
        if not self.user.GetCursorPos(ctypes.byref(point)):
            raise OSError("Desktop pointer is unavailable")
        return point.x, point.y

    def place(self, x, y):
        if not self.ready() or not self.user.SetCursorPos(x, y):
            return False
        return self.pointer() == (x, y)

    def ready(self):
        desk = self.user.OpenInputDesktop(0, False, 1)
        if not desk:
            return False
        try:
            name, required = ctypes.create_unicode_buffer(256), self.w.DWORD()
            return bool(self.user.GetUserObjectInformationW(desk, 2, name, ctypes.sizeof(name), ctypes.byref(required))) and name.value == "Default"
        finally:
            self.user.CloseDesktop(desk)


class X11Desktop:
    name = "Linux X11"

    def __init__(self):
        library = ctypes.util.find_library("X11")
        if not library:
            raise UnsupportedDesktop("Install libX11 and xrandr for X11 integration")
        self.x = ctypes.CDLL(library)
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]
        self.x.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_int]
        self.x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise UnsupportedDesktop("An accessible X11 desktop session is required")
        self.root = self.x.XDefaultRootWindow(self.display)
        self.lock_checked, self.unlocked = 0, False

    def screens(self):
        output = subprocess.check_output(["xrandr", "--listactivemonitors"], text=True, timeout=3,
                                         env=_system_environment())
        rects = []
        for line in output.splitlines()[1:]:
            match = re.search(r"(\d+)/\d+x(\d+)/\d+([+-]\d+)([+-]\d+)", line)
            if match:
                w, h, x, y = map(int, match.groups())
                rects.append(Rect(x, y, w, h))
        if not rects:
            raise UnsupportedDesktop("xrandr did not report active monitor geometry")
        return sorted(set(rects), key=lambda r: (r.x, r.y, r.width, r.height))

    def pointer(self):
        root, child = ctypes.c_ulong(), ctypes.c_ulong()
        x, y, wx, wy, mask = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint()
        if not self.x.XQueryPointer(self.display, self.root, ctypes.byref(root), ctypes.byref(child),
            ctypes.byref(x), ctypes.byref(y), ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask)):
            raise OSError("Cannot query X11 pointer")
        return x.value, y.value

    def place(self, x, y):
        self.lock_checked = 0
        if not self.ready():
            return False
        self.x.XWarpPointer(self.display, 0, self.root, 0, 0, 0, 0, x, y)
        self.x.XSync(self.display, False)
        return self.pointer() == (x, y)

    def ready(self):
        if time.monotonic() - self.lock_checked < 1:
            return self.unlocked
        self.lock_checked = time.monotonic()
        self.unlocked = False
        # Fail closed if the session manager cannot establish desktop readiness.
        session = os.environ.get("XDG_SESSION_ID")
        if not session:
            return False
        try:
            info = subprocess.check_output(["loginctl", "show-session", session,
                "-p", "Active", "-p", "LockedHint"], text=True, timeout=2, env=_system_environment())
            self.unlocked = "Active=yes" in info and "LockedHint=no" in info
            desktop = os.environ.get("XDG_CURRENT_DESKTOP", "").lower()
            interfaces = {"cinnamon": ("org.cinnamon.ScreenSaver", "/org/cinnamon/ScreenSaver"),
                          "mate": ("org.mate.ScreenSaver", "/org/mate/ScreenSaver"),
                          "xfce": ("org.xfce.ScreenSaver", "/org/xfce/ScreenSaver"),
                          "gnome": ("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver")}
            for name, (bus, path) in interfaces.items():
                if name not in desktop or not self.unlocked:
                    continue
                active = subprocess.check_output(["gdbus", "call", "--session", "--dest", bus,
                    "--object-path", path, "--method", bus + ".GetActive"], text=True, timeout=2,
                    stderr=subprocess.DEVNULL, env=_system_environment())
                self.unlocked = active.strip() == "(false,)"
                break
        except (OSError, subprocess.SubprocessError):
            self.unlocked = False
        return self.unlocked


def create_desktop():
    if sys.platform == "win32":
        return WindowsDesktop()
    if sys.platform.startswith("linux"):
        if os.environ.get("XDG_SESSION_TYPE") == "wayland" or os.environ.get("WAYLAND_DISPLAY"):
            raise UnsupportedDesktop("Wayland edge capture/placement requires a compositor portal backend; XWayland is not a substitute. CLI configuration remains available.")
        return X11Desktop()
    raise UnsupportedDesktop("Automatic switching currently supports Windows and Linux X11")
