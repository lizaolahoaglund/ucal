"""Read or control one owned CANoe measurement using its UI Automation controls."""
import argparse
import hashlib
import json
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--action', required=True, choices=('State', 'Start', 'Stop'))
    args = parser.parse_args()
    import psutil
    import win32gui
    import win32process
    from pywinauto import Application

    process = psutil.Process(args.pid)
    executable = args.executable.resolve()
    if executable.name.lower() != 'canoe64.exe' or Path(process.exe()).resolve() != executable:
        raise RuntimeError('The owned CANoe executable identity does not match.')
    created = process.create_time()
    windows = []

    def collect(handle, _):
        if (win32process.GetWindowThreadProcessId(handle)[1] == args.pid
                and win32gui.IsWindowVisible(handle)
                and 'CANoe' in win32gui.GetWindowText(handle)
                and win32gui.GetClassName(handle).startswith('WindowsForms10.Window.')):
            windows.append(handle)

    win32gui.EnumWindows(collect, None)
    if len(windows) != 1 or not win32gui.IsWindowEnabled(windows[0]):
        raise RuntimeError('Expected one enabled CANoe main window without a blocking modal.')
    window = Application(backend='uia').connect(process=args.pid).window(handle=windows[0])

    def controls():
        found = {}
        for action in ('Start', 'Stop'):
            matches = [button for button in window.descendants(control_type='Button')
                       if button.window_text() == action
                       and button.is_visible()
                       and button.parent().window_text() == 'Quick Access Toolbar']
            if len(matches) != 1:
                raise RuntimeError(f'Expected one owned CANoe quick-access {action} control.')
            found[action] = matches[0]
        return found

    def running(buttons):
        start, stop = buttons['Start'].is_enabled(), buttons['Stop'].is_enabled()
        if start == stop:
            return None
        return stop

    buttons = controls()
    state = running(buttons)
    if state is None:
        raise RuntimeError('CANoe measurement controls have an indeterminate state.')
    if args.action != 'State' and state != (args.action == 'Start'):
        buttons[args.action].invoke()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = running(controls())
            if state == (args.action == 'Start'):
                break
            time.sleep(.1)
        if state != (args.action == 'Start'):
            raise RuntimeError(f'CANoe did not enter the requested {args.action} state.')
    current = psutil.Process(args.pid)
    if current.create_time() != created or Path(current.exe()).resolve() != executable:
        raise RuntimeError('The owned CANoe process changed during measurement control.')
    print(json.dumps({'status': 'passed', 'pid': args.pid, 'window': windows[0],
                      'executable': str(executable), 'sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                      'action': args.action, 'running': state}))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(json.dumps({'status': 'failed', 'error': str(error)}))
        raise SystemExit(1)
