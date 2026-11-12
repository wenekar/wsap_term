import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require


def ask(path, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        connection.sendall(command.encode() + b'\n')
        reply = json.loads(connection.makefile('rb').readline())
    assert reply.get('ok'), reply
    return reply['result']


def eventually(predicate, message, timeout=10):
    deadline = time.monotonic() + timeout
    error = None
    while time.monotonic() < deadline:
        while GLib.MainContext.default().pending():
            GLib.MainContext.default().iteration(False)
        try:
            result = predicate()
            if result:
                return result
        except (GLib.Error, OSError) as caught:
            error = caught
        time.sleep(0.02)
    raise AssertionError(f'{message}: {error}' if error else message)


def set_enabled(enabled):
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    connection.call_sync(
        'org.a11y.Bus', '/org/a11y/bus',
        'org.freedesktop.DBus.Properties', 'Set',
        GLib.Variant('(ssv)', ('org.a11y.Status', 'IsEnabled',
                               GLib.Variant('b', enabled))),
        None, Gio.DBusCallFlags.NONE, 5000, None)


def walk(root):
    pending = [root]
    while pending:
        node = pending.pop()
        yield node
        pending.extend(node.get_child_at_index(index)
                       for index in reversed(range(node.get_child_count())))


def find(root, name):
    return next((node for node in walk(root) if node.get_name() == name), None)


def application(pid):
    desktop = Atspi.get_desktop(0)
    return next((desktop.get_child_at_index(index)
                 for index in range(desktop.get_child_count())
                 if desktop.get_child_at_index(index).get_process_id() == pid), None)


def text(node):
    return Atspi.Text.get_text(node, 0, -1)


def selection(node):
    if Atspi.Text.get_n_selections(node) != 1:
        return None
    span = Atspi.Text.get_selection(node, 0)
    return span.start_offset, span.end_offset


def run_session():
    set_enabled(False)
    Atspi.init()
    Atspi.set_timeout(1000, 1000)
    try:
        with HeadlessSway() as sway:
            sway.env['WSAP_RENDER_GPU'] = '0'
            debug = sway.work / 'accessibility.sock'
            terminal = sway.terminal(
                ['command = /bin/cat', 'width = 1000', 'height = 800',
                 'cursor-blink = false'],
                '--debug-socket', str(debug))
            eventually(lambda: ask(debug, 'windows'),
                       'the terminal did not start')

            # Enable the service after the first frame.
            set_enabled(True)
            app = eventually(lambda: application(terminal.pid),
                             'AT-SPI did not expose the running app')
            view = eventually(lambda: find(app, 'Terminal'),
                              'AT-SPI did not expose the terminal')
            assert view.get_role() == Atspi.Role.TERMINAL, view.get_role_name()
            ask(debug, 'text "AccessKit α😀"')
            ask(debug, 'key enter')
            eventually(lambda: 'AccessKit α😀' in text(view),
                       'AT-SPI did not publish Unicode terminal output')
            lint = ask(debug, 'lint')
            assert lint['count'] == 0, lint

            ask(debug, 'shortcut settings')
            eventually(lambda: len(ask(debug, 'windows')) == 2,
                       'the settings window did not open')
            entry = eventually(lambda: find(app, 'Search settings'),
                               'AT-SPI did not expose the settings input')
            assert entry.get_state_set().contains(Atspi.StateType.EDITABLE)
            assert Atspi.Component.grab_focus(entry)
            eventually(lambda: entry.get_state_set().contains(Atspi.StateType.FOCUSED),
                       'the accessibility focus action did not reach the input')

            value = 'a😀éz'
            assert Atspi.EditableText.set_text_contents(entry, value)
            eventually(lambda: text(entry) == value,
                       'the accessibility value action did not edit the input')
            assert Atspi.Text.get_character_count(entry) == len(value)
            assert Atspi.Text.set_caret_offset(entry, 3)
            eventually(lambda: Atspi.Text.get_caret_offset(entry) == 3,
                       'the accessibility caret action used the wrong text unit')
            assert Atspi.Text.add_selection(entry, 1, 3)
            eventually(lambda: selection(entry) == (1, 3),
                       'the accessibility selection used the wrong text unit')
            assert Atspi.Text.get_text(entry, 1, 3) == '😀é'
            assert Atspi.Text.set_selection(entry, 0, 2, 4)
            eventually(lambda: selection(entry) == (2, 4),
                       'the accessibility selection update did not reach the input')
            lint = ask(debug, 'lint -w 1')
            assert lint['count'] == 0, lint

            cancel = eventually(lambda: find(app, 'Cancel'),
                                'AT-SPI did not expose the Cancel button')
            assert cancel.get_role() == Atspi.Role.PUSH_BUTTON
            assert Atspi.Action.get_n_actions(cancel) > 0
            assert Atspi.Action.do_action(cancel, 0)
            eventually(lambda: len(ask(debug, 'windows')) == 1,
                       'the accessibility button action did not close settings')
            eventually(lambda: find(app, 'Search settings') is None,
                       'AT-SPI kept the closed settings window')
            assert terminal.poll() is None
            print('PASS: AT-SPI activation, Unicode text, focus, selection, and actions')
    finally:
        Atspi.exit()


def main():
    require('sway', 'dbus-run-session')
    try:
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi as atspi, Gio as gio, GLib as glib
    except (ImportError, ValueError):
        print('SKIP: needs Python GI and the AT-SPI typelib')
        return 77
    global Atspi, Gio, GLib
    Atspi, Gio, GLib = atspi, gio, glib
    if '--session' in sys.argv:
        run_session()
        return 0

    # Keep the test off the user's accessibility bus.
    env = dict(os.environ)
    env.pop('AT_SPI_BUS_ADDRESS', None)
    env.pop('WSAP_ACCESSIBILITY', None)
    env['GSETTINGS_BACKEND'] = 'memory'
    env['ATSPI_DBUS_IMPLEMENTATION'] = 'dbus-daemon'
    return subprocess.run(
        ['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
         '--session'], env=env, timeout=90).returncode


if __name__ == '__main__':
    sys.exit(main())
