"""Negative controls for the installed-product acceptance observer."""
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Event
import time
import unittest

import win32con
import win32gui

from lifecycle import Lifecycle, windows_for


class LifecycleTest(unittest.TestCase):
    def observe(self, source, assert_result, expected_window=lambda *args: False):
        with TemporaryDirectory() as folder:
            evidence = Path(folder) / 'events.jsonl'
            with Lifecycle(evidence, (), expected_window) as observer:
                # Wait for the monitor's process handle before the test action.
                process = subprocess.Popen([sys.executable, '-c', 'input()\n' + source],
                                           stdin=subprocess.PIPE, text=True)
                try:
                    observer.root(process.pid, application=True)
                    process.stdin.write('\n')
                    process.stdin.flush()
                    assert_result(observer)
                finally:
                    observer.set_phase('cleanup')
                    if process.poll() is None:
                        process.terminate()
                    process.wait(5)
                    process.stdin.close()
            return [json.loads(line) for line in evidence.read_text(encoding='utf-8').splitlines()]

    def expect_failure(self, observer, name):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if any(event['event'] == name for event in observer.failures):
                with self.assertRaises(RuntimeError):
                    observer.check()
                return
            time.sleep(.05)
        self.fail(f'Observer did not report {name}')

    def test_late_dialogs_are_failures_even_if_traffic_finished(self):
        for message in ('Not enough timers available', 'The parameter is incorrect'):
            with self.subTest(message=message):
                events = self.observe(
                    f'import ctypes,time; time.sleep(.35); '
                    f'ctypes.windll.user32.MessageBoxW(0, {message!r}, "Fixture error", 16)',
                    lambda observer: (observer.set_phase('running'),
                                      self.expect_failure(observer, 'unexpected-dialog')))
                failure = next(event for event in events if event['event'] == 'unexpected-dialog')
                self.assertIn(message, failure['text'])
                self.assertEqual('running', failure['phase'])

    def check_startup_dialog_outside_startup(self, hide_before_running):
        title = 'Fixture startup dialog'
        observed = Event()

        def expected_window(_name, window_title, kind, phase):
            if window_title == title and kind == '#32770' and phase == 'startup':
                observed.set()
                return True
            return False

        def exercise(observer):
            self.assertTrue(observed.wait(5), 'The observer did not allow the startup dialog')
            observer.check()
            pid, = observer.roots
            handle, = [window for window in windows_for(pid) if win32gui.GetWindowText(window) == title]
            if hide_before_running:
                win32gui.ShowWindow(handle, win32con.SW_HIDE)
                self.assertFalse(win32gui.IsWindowVisible(handle))
            observer.set_phase('running')
            if hide_before_running:
                observer._scan()
                observer.check()
                win32gui.ShowWindow(handle, win32con.SW_SHOWNOACTIVATE)
                self.assertIn(handle, windows_for(pid))
            self.expect_failure(observer, 'unexpected-dialog')
            observer._scan()

        events = self.observe(
            f'import ctypes; ctypes.windll.user32.MessageBoxW(0, "Fixture message", {title!r}, 16)',
            exercise, expected_window)
        self.assertEqual(
            [('startup', 'expected-window'), ('running', 'unexpected-dialog')],
            [(event['phase'], event['event']) for event in events if event.get('title') == title])

    def test_startup_dialog_remaining_visible_in_running_fails(self):
        self.check_startup_dialog_outside_startup(hide_before_running=False)

    def test_same_startup_dialog_reappearing_in_running_fails(self):
        self.check_startup_dialog_outside_startup(hide_before_running=True)

    def test_unexpected_exit_cannot_pass_even_with_zero_exit_code(self):
        for code in (0, 17):
            with self.subTest(code=code):
                events = self.observe(f'raise SystemExit({code})',
                                      lambda observer: self.expect_failure(observer, 'unexpected-process-exit'))
                self.assertIn(code, [event['exitCode'] for event in events
                                     if event['event'] == 'unexpected-process-exit'])

    def test_nonzero_exit_during_requested_close_fails(self):
        def exercise(observer):
            observer.set_phase('closing')
            with self.assertRaisesRegex(RuntimeError, 'unexpected-process-exit'):
                observer.wait_for_application_exit()

        events = self.observe('import time; time.sleep(.3); raise SystemExit(17)', exercise)
        self.assertIn(17, [event['exitCode'] for event in events
                           if event['event'] == 'unexpected-process-exit'])

    def test_timeout_and_forced_cleanup_cannot_pass(self):
        def exercise(observer):
            observer.set_phase('closing')
            with self.assertRaisesRegex(RuntimeError, 'application-exit-timeout'):
                observer.wait_for_application_exit(timeout=.3)
            observer.terminate_after_failure()
        events = self.observe('import time; time.sleep(30)', exercise)
        self.assertIn('forced-termination', [event['event'] for event in events])

    def test_requested_zero_exit_passes(self):
        def exercise(observer):
            observer.set_phase('closing')
            observer.wait_for_application_exit()
            observer.check()
        self.observe('import time; time.sleep(.3)', exercise)


if __name__ == '__main__':
    unittest.main()
