"""Observe an installed application's entire run on the interactive Windows desktop."""
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
from pathlib import Path
from threading import Event, RLock, Thread
import time

import psutil
import win32api
import win32con
import win32event
import win32gui
import win32process


def windows_for(pid):
    windows = []

    def collect(handle, _):
        if win32process.GetWindowThreadProcessId(handle)[1] == pid and win32gui.IsWindowVisible(handle):
            windows.append(handle)

    win32gui.EnumWindows(collect, None)
    return windows


def close_main_window(handle, pid):
    if (not win32gui.IsWindow(handle) or not win32gui.IsWindowEnabled(handle)
            or win32process.GetWindowThreadProcessId(handle)[1] != pid):
        raise RuntimeError('The owned main window is unavailable or blocked.')
    win32gui.PostMessage(handle, win32con.WM_CLOSE, 0, 0)


def window_details(handle):
    controls = []
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    get_text = user32.SendMessageTimeoutW
    get_text.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                         wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    get_text.restype = wintypes.LPARAM

    def collect(child, _):
        if not win32gui.IsWindowVisible(child) or len(controls) >= 16:
            return
        buffer = ctypes.create_unicode_buffer(512)
        size = ctypes.c_size_t()
        if get_text(child, win32con.WM_GETTEXT, len(buffer), ctypes.addressof(buffer),
                    win32con.SMTO_ABORTIFHUNG, 50, ctypes.byref(size)) and buffer.value:
            controls.append(buffer.value)

    win32gui.EnumChildWindows(handle, collect, None)
    return controls


class Lifecycle:
    def __init__(self, evidence, application_names, expected_window):
        self.names = {name.casefold() for name in application_names}
        self.expected_window = expected_window
        self.phase = 'startup'
        self.roots = set()
        self.processes = {}
        self.logged_window_events = set()
        self.failures = []
        self.done = Event()
        self.ready = Event()
        self.lock = RLock()
        self.log = Path(evidence).open('x', encoding='utf-8', buffering=1)
        self.worker = Thread(target=self._watch, daemon=True)

    def __enter__(self):
        self.worker.start()
        if not self.ready.wait(5):
            self.fail('observer-timeout')
        self.check()
        return self

    def __exit__(self, *exc):
        self.done.set()
        self.worker.join(5)
        if self.worker.is_alive():
            self.fail('observer-stop-timeout')
        else:
            for item in self.processes.values():
                item['handle'].Close()
            self.log.close()

    def record(self, kind, **details):
        with self.lock:
            event = {'time': datetime.now(timezone.utc).isoformat(), 'phase': self.phase,
                     'event': kind, **details}
            self.log.write(json.dumps(event, ensure_ascii=False) + '\n')
            return event

    def fail(self, kind, **details):
        with self.lock:
            self.failures.append(self.record(kind, **details))

    def check(self):
        with self.lock:
            if self.failures:
                first = self.failures[0]
                raise RuntimeError(f"Installed acceptance failed: {first['event']} {first.get('title', '')}")

    def root(self, pid, *, application=False):
        with self.lock:
            self.roots.add(pid)
            self._attach(psutil.Process(pid), application=application)

    def set_phase(self, phase):
        with self.lock:
            self.phase = phase
            self.record('phase')

    def _attach(self, process, application):
        if process.pid in self.processes:
            return
        # A handle keeps the original process identity and exit code available after exit.
        handle = win32api.OpenProcess(0x1000 | win32con.SYNCHRONIZE, False, process.pid)
        self.processes[process.pid] = {'handle': handle, 'application': application,
                                       'name': process.name(), 'exited': False}
        self.record('process-started', pid=process.pid, name=process.name())

    def _window(self, handle):
        if not win32gui.IsWindow(handle) or not win32gui.IsWindowVisible(handle):
            return
        pid = win32process.GetWindowThreadProcessId(handle)[1]
        process = self.processes.get(pid)
        if not process or process['exited']:
            return
        title, kind = win32gui.GetWindowText(handle), win32gui.GetClassName(handle)
        owner = win32gui.GetWindow(handle, win32con.GW_OWNER)
        style = win32gui.GetWindowLong(handle, win32con.GWL_EXSTYLE)
        manager_splash = (process['name'].casefold() == 'ucalmanager.exe'
                          and title == 'Ucal Manager' and kind == 'Qt5QWindowIcon'
                          and not window_details(handle))
        if manager_splash or self.expected_window(process['name'].casefold(), title, kind, self.phase):
            event = 'expected-window'
        elif (kind == '#32770' or 'dialog' in kind.casefold() or 'messageform' in kind.casefold()
              or style & win32con.WS_EX_DLGMODALFRAME or (owner and not win32gui.IsWindowEnabled(owner))):
            event = 'unexpected-dialog'
        else:
            return

        # Recheck the current phase before deduplicating the observed event.
        key = (pid, handle, title, kind, event)
        if key in self.logged_window_events:
            return
        self.logged_window_events.add(key)
        if event == 'unexpected-dialog':
            self.fail(event, pid=pid, title=title, windowClass=kind, text=window_details(handle))
        else:
            self.record(event, pid=pid, title=title, windowClass=kind)

    def _scan(self):
        with self.lock:
            for pid in self.roots:
                try:
                    children = psutil.Process(pid).children(recursive=True)
                except psutil.NoSuchProcess:
                    continue
                for child in children:
                    try:
                        if child.name().casefold() in self.names:
                            self._attach(child, application=True)
                    except psutil.NoSuchProcess:
                        continue
            for pid, process in self.processes.items():
                if process['exited']:
                    continue
                if win32event.WaitForSingleObject(process['handle'], 0) == win32event.WAIT_OBJECT_0:
                    code = win32process.GetExitCodeProcess(process['handle'])
                    process.update(exited=True, exitCode=code)
                    details = {'pid': pid, 'name': process['name'], 'exitCode': code}
                    self.record('process-exited', **details)
                    if code != 0 or self.phase not in ('closing', 'cleanup'):
                        self.fail('unexpected-process-exit', **details)
                else:
                    for handle in windows_for(pid):
                        self._window(handle)

    def _watch(self):
        try:
            self._scan()
            self.ready.set()
            while not self.done.wait(.05):
                self._scan()
            self._scan()
        except Exception as error:
            self.fail('observer-error', error=str(error))
            self.ready.set()

    def wait_for_application_exit(self, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.check()
            with self.lock:
                apps = [p for p in self.processes.values() if p['application']]
                if apps and all(p['exited'] and p['exitCode'] == 0 for p in apps):
                    return
            time.sleep(.05)
        self.fail('application-exit-timeout')
        self.check()

    def terminate_after_failure(self):
        with self.lock:
            applications = [(pid, item) for pid, item in self.processes.items()
                            if item['application']]
        # Give the supervisor a chance to release its temporary driver resource
        # after the application has gone, even on an already failed run.
        applications.sort(key=lambda entry: entry[1]['name'].casefold().endswith('bridge.exe'))
        for pid, item in applications:
            timeout = 4000 if item['name'].casefold().endswith('bridge.exe') else 0
            if win32event.WaitForSingleObject(item['handle'], timeout) == win32event.WAIT_OBJECT_0:
                continue
            self.fail('forced-termination', pid=pid, name=item['name'])
            try:
                process = psutil.Process(pid)
                process.terminate()
                process.wait(5)
            except psutil.NoSuchProcess:
                pass
