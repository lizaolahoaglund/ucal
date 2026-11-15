"""Create and launch one profile through the normal Manager UI."""
import json
import time

from pywinauto import Desktop, keyboard, mouse
import win32gui
import win32process
from lifecycle import windows_for


def wait_for(lifecycle, description, find, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lifecycle.check()
        value = find()
        if value is not None:
            return value
        time.sleep(.15)
    raise RuntimeError(f'Timed out waiting for {description}')


def manager_window(pid):
    for handle in windows_for(pid):
        if win32gui.GetWindowText(handle) == 'UCAL Manager':
            return Desktop(backend='uia').window(handle=handle).wrapper_object()
    return None


def create_and_launch(pid, data, software, lifecycle, *, endpoint_id=None,
                      endpoint_label=None, configuration=None):
    def one(parent, kind, title):
        matches = [item for item in parent.descendants(control_type=kind)
                   if item.window_text() == title and item.is_visible() and item.is_enabled()]
        return matches[0] if len(matches) == 1 else None

    def find_button(parent, title):
        return wait_for(lifecycle, title, lambda: one(parent, 'Button', title))

    def tab(window, name):
        control = wait_for(lifecycle, name, lambda: one(window, 'CheckBox', name))
        control.invoke()

    def saved():
        path = data/'profiles.json'
        if path.exists():
            return json.loads(path.read_text(encoding='utf-8')).get('applications', [])
        return []

    window = wait_for(lifecycle, 'Manager main window', lambda: manager_window(pid))
    if saved():
        raise RuntimeError('Manager acceptance must start without a saved application')
    tab(window, 'Software')
    software_path = str(software).replace('\\', '/').casefold()

    def software_card():
        for text in window.descendants(control_type='Text'):
            if text.window_text().replace('\\', '/').casefold() == software_path:
                parent = text.parent()
                for _ in range(4):
                    button = one(parent, 'Button', 'Create application')
                    if button is not None:
                        return button
                    parent = parent.parent()
        return None

    create = wait_for(lifecycle, f'scanned software {software.name}', software_card)
    lifecycle.set_phase('create-application')
    create.invoke()
    dialog = wait_for(lifecycle, 'Create application dialog',
                      lambda: one(window, 'Window', 'Create application'))
    assignment = 'Selected channels' if endpoint_label else 'All compatible channels'
    wait_for(lifecycle, assignment, lambda: one(dialog, 'RadioButton', assignment)).invoke()
    if endpoint_label:
        # Qt's UI Automation provider exposes the table cell but may omit its
        # embedded combo box. Locate the selector from the live row and column.
        column = wait_for(lifecycle, 'Hardware endpoint column',
                          lambda: one(dialog, 'Header', 'Hardware endpoint')).rectangle()
        row = wait_for(lifecycle, 'CAN 1 row', lambda: one(dialog, 'DataItem', 'CAN 1')).rectangle()
        mouse.click(coords=(column.right - 14, (row.top + row.bottom) // 2))

        def option_index():
            for handle in windows_for(pid):
                popup = Desktop(backend='uia').window(handle=handle).wrapper_object()
                for kind in ('ListItem', 'DataItem'):
                    items = [item for item in popup.descendants(control_type=kind)
                             if item.is_visible() and item.is_enabled()]
                    matches = [index for index, item in enumerate(items)
                               if item.window_text() == endpoint_label]
                    if len(matches) == 1:
                        return matches[0]
            return None

        index = wait_for(lifecycle, f'channel {endpoint_label}', option_index)
        # Keyboard navigation uses the popup's list order. Qt can report a
        # different screen rectangle for a popup row than its mouse hit area.
        if win32process.GetWindowThreadProcessId(win32gui.GetForegroundWindow())[1] != pid:
            raise RuntimeError('The owned Manager channel picker does not have keyboard focus')
        keyboard.send_keys('{HOME}' + '{DOWN}' * index + '{ENTER}')
    if configuration:
        tabs = [item for item in dialog.descendants(control_type='TabItem')
                if item.window_text().startswith('Integration')]
        if len(tabs) != 1:
            raise RuntimeError('Could not locate the integration settings tab')
        tabs[0].invoke()
        def configuration_fields():
            edits = [item for item in dialog.descendants(control_type='Edit')
                     if item.is_visible() and item.is_enabled()]
            return edits if len(edits) == 3 else None

        edits = wait_for(lifecycle, 'configuration file field', configuration_fields)
        edits[1].set_edit_text(str(configuration))
    find_button(dialog, 'Save application').invoke()
    applications = wait_for(lifecycle, 'one saved application',
                            lambda: (items if len(items := saved()) == 1 else None))
    app = applications[0]
    if endpoint_id and [m['endpointId'] for m in app['mappings']] != [endpoint_id]:
        raise RuntimeError('The saved application did not bind the requested physical endpoint')
    if configuration and app['configurationPath'] != str(configuration):
        raise RuntimeError('The saved application did not use the requested CANoe configuration')
    lifecycle.record('application-created', id=app['id'], software=app['softwareInstallationId'])
    tab(window, 'Applications')
    launch = find_button(window, 'Launch')
    lifecycle.set_phase('startup')
    launch.invoke()
    return app


def expected_manager_window(name, title, kind, phase):
    return (name == 'ucalmanager.exe' and title == 'Create application'
            and kind == 'Qt5QWindowIcon' and phase == 'create-application')
