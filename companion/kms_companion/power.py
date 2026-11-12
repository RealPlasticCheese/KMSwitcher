"""OS power notifications, independent of the tray and desktop readiness."""
from enum import IntEnum
import threading
import sys


class PowerState(IntEnum):
    UNKNOWN = 0
    AWAKE = 1
    SLEEP_OR_HIBERNATE = 2
    SHUTDOWN = 3


def power_label(state, fresh):
    labels = {"awake": "Awake", "sleep_or_hibernate": "Sleep / hibernate requested",
              "shutdown": "Shutdown / restart requested", "unknown": "Unknown"}
    label = labels.get(state, "Unknown")
    return label if fresh or state == "unknown" else "Last reported: " + label


def windows_event(message, wparam, lparam):
    if message == 0x218:  # WM_POWERBROADCAST
        if wparam == 4:  # PBT_APMSUSPEND: Windows does not identify the target sleep state.
            return PowerState.SLEEP_OR_HIBERNATE
        if wparam in (7, 18):  # PBT_APMRESUMESUSPEND / PBT_APMRESUMEAUTOMATIC
            return PowerState.AWAKE
    if message == 0x11 and not lparam & 0x80000001:  # WM_QUERYENDSESSION; exclude logoff/Restart Manager.
        return PowerState.SHUTDOWN
    if message == 0x16 and not wparam:  # WM_ENDSESSION: shutdown cancelled.
        return PowerState.AWAKE
    return None


class PowerMonitor:
    def __init__(self, notify):
        self.notify = notify
        self.reason = "Starting power notifications"
        self.stopped = threading.Event()
        self.started = threading.Event()
        self.window = None
        self.loop = None
        self.bus = None
        self.inhibitor = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="kms-power", daemon=True)
        self.thread.start()

    def close(self):
        self.stopped.set()
        if self.window:
            import ctypes
            ctypes.windll.user32.PostMessageW(ctypes.c_void_p(self.window), 0x10, 0, 0)
        if self.bus and self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(self.bus.disconnect)
        if hasattr(self, "thread"):
            self.thread.join(timeout=2)

    def _run(self):
        try:
            if sys.platform == "win32":
                self._windows()
            elif sys.platform.startswith("linux"):
                import asyncio
                asyncio.run(self._linux())
            else:
                raise OSError("Power notifications are unavailable on this platform")
        except Exception as error:
            self.reason = "Power notifications unavailable: " + str(error)
            if not self.stopped.is_set():
                self.notify(PowerState.UNKNOWN)
        finally:
            self.started.set()

    def _windows(self):
        import ctypes as c
        from ctypes import wintypes as w
        user = c.WinDLL("user32", use_last_error=True)
        kernel = c.WinDLL("kernel32", use_last_error=True)
        proc_type = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)

        class WindowClass(c.Structure):
            _fields_ = [("style", w.UINT), ("proc", proc_type), ("class_extra", c.c_int),
                        ("window_extra", c.c_int), ("instance", w.HINSTANCE), ("icon", w.HICON),
                        ("cursor", w.HANDLE), ("background", w.HBRUSH),
                        ("menu", w.LPCWSTR), ("name", w.LPCWSTR)]

        kernel.GetModuleHandleW.argtypes = [w.LPCWSTR]
        kernel.GetModuleHandleW.restype = w.HMODULE
        user.RegisterClassW.argtypes = [c.POINTER(WindowClass)]
        user.RegisterClassW.restype = w.ATOM
        user.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
                                        c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
        user.CreateWindowExW.restype = w.HWND
        user.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        user.DefWindowProcW.restype = c.c_ssize_t
        user.GetMessageW.argtypes = [c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT]
        user.DispatchMessageW.argtypes = [c.POINTER(w.MSG)]
        user.DispatchMessageW.restype = c.c_ssize_t
        user.DestroyWindow.argtypes = [w.HWND]
        user.UnregisterClassW.argtypes = [w.LPCWSTR, w.HINSTANCE]
        user.RegisterSuspendResumeNotification.argtypes = [w.HANDLE, w.DWORD]
        user.RegisterSuspendResumeNotification.restype = w.HANDLE
        user.UnregisterSuspendResumeNotification.argtypes = [w.HANDLE]

        @proc_type
        def dispatch(hwnd, message, wp, lp):
            state = windows_event(message, wp, lp)
            if state is not None:
                self.notify(state)  # Bounded wait for the HID owner's local acknowledgement.
            if message in (0x218, 0x11):
                return 1
            if message == 2:  # WM_DESTROY
                user.PostQuitMessage(0)
                return 0
            return user.DefWindowProcW(hwnd, message, wp, lp)

        name = "KMSwitcherPower" + str(id(self))
        instance = kernel.GetModuleHandleW(None)
        definition = WindowClass(proc=dispatch, instance=instance, name=name)
        if not user.RegisterClassW(c.byref(definition)):
            raise c.WinError(c.get_last_error())
        registration = None
        try:
            # A hidden top-level window receives shutdown broadcasts; a message-only window does not.
            self.window = user.CreateWindowExW(0, name, name, 0, 0, 0, 0, 0, None, None, instance, None)
            if not self.window:
                raise c.WinError(c.get_last_error())
            registration = user.RegisterSuspendResumeNotification(self.window, 0)
            if not registration:
                raise c.WinError(c.get_last_error())
            self.reason = "Windows power notifications"
            self.notify(PowerState.AWAKE)
            self.started.set()
            message = w.MSG()
            while not self.stopped.is_set():
                result = user.GetMessageW(c.byref(message), None, 0, 0)
                if result == -1:
                    raise c.WinError(c.get_last_error())
                if result == 0:
                    break
                user.TranslateMessage(c.byref(message))
                user.DispatchMessageW(c.byref(message))
        finally:
            if registration:
                user.UnregisterSuspendResumeNotification(registration)
            if self.window:
                user.DestroyWindow(self.window)
                self.window = None
            user.UnregisterClassW(name, instance)

    async def _linux(self):
        import asyncio
        import os
        from dbus_next import BusType, Message, MessageType
        from dbus_next.aio import MessageBus
        self.loop = asyncio.get_running_loop()
        bus = self.bus = await MessageBus(bus_type=BusType.SYSTEM, negotiate_unix_fd=True).connect()
        tasks = set()
        transition_lock = asyncio.Lock()

        async def inhibit():
            self.inhibitor = []
            # Some logind policies reject a combined request even when each
            # individual delay permission is granted. Keep both requests explicit.
            for action in ("sleep", "shutdown"):
                reply = await bus.call(Message(destination="org.freedesktop.login1", path="/org/freedesktop/login1",
                    interface="org.freedesktop.login1.Manager", member="Inhibit", signature="ssss",
                    body=[action, "KMSwitcher", "Report power state to the USB receiver", "delay"]))
                if reply.message_type == MessageType.ERROR:
                    raise OSError(reply.body[0])
                self.inhibitor.append(reply.unix_fds[reply.body[0]])

        def release():
            if self.inhibitor is not None:
                for descriptor in self.inhibitor:
                    os.close(descriptor)
                self.inhibitor = None

        async def transition(member, entering):
            async with transition_lock:
                await report_transition(member, entering)

        async def report_transition(member, entering):
            if entering:
                try:
                    await asyncio.to_thread(self.notify, PowerState.SLEEP_OR_HIBERNATE if member == "PrepareForSleep" else PowerState.SHUTDOWN)
                finally:
                    release()  # Never hold up sleep after our bounded reporting attempt.
            else:
                await inhibit()
                await asyncio.to_thread(self.notify, PowerState.AWAKE)

        def received(message):
            if (message.message_type == MessageType.SIGNAL and message.path == "/org/freedesktop/login1" and
                    message.interface == "org.freedesktop.login1.Manager" and
                    message.member in ("PrepareForSleep", "PrepareForShutdown")):
                task = asyncio.create_task(transition(message.member, message.body[0]))
                tasks.add(task)

        try:
            bus.add_message_handler(received)
            reply = await bus.call(Message(destination="org.freedesktop.DBus", path="/org/freedesktop/DBus",
                interface="org.freedesktop.DBus", member="AddMatch", signature="s",
                body=["type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager'"]))
            if reply.message_type == MessageType.ERROR:
                raise OSError(reply.body[0])
            await inhibit()
            self.reason = "Linux logind power notifications"
            await asyncio.to_thread(self.notify, PowerState.AWAKE)
            self.started.set()
            # Observe task failures as well as loss of logind's bus connection.
            while not self.stopped.is_set() and bus.connected:
                for task in tuple(tasks):
                    if task.done():
                        tasks.remove(task)
                        task.result()
                await asyncio.sleep(.1)
            if not self.stopped.is_set():
                raise OSError("System bus disconnected")
        finally:
            for task in tasks:
                task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            release()
            bus.disconnect()
