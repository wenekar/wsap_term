"""A context menu that the compositor dismisses must close, and only the menu:
the window that opened it stays. The compositor dismisses a menu when a click
lands on another client, so a second terminal sits beside the first. Runs a
private headless Sway with a virtual pointer, and checks the menu through the
inspector socket."""
import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for  # noqa: E402


def ask(path, line):
    """One inspector request; None when the terminal does not answer."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(3)
            s.connect(str(path))
            s.sendall(line.encode() + b'\n')
            reply = s.makefile('rb').readline()
    except OSError:
        return None
    return json.loads(reply) if reply else None


def menu_open(path):
    """True while menu level 0 shows in a native popup."""
    reply = ask(path, 'tree -p 0 --depth 1')
    return bool(reply and reply.get('ok'))


def main():
    require('sway')
    with HeadlessSway() as sway:
        # Pointer positions are thousandths of the output, whatever its size.
        pointer = sway.device('pointer', '1000', '1000')
        debug = sway.work / 'debug.sock'
        config = ['command = /bin/cat', 'width = 800', 'height = 600']
        terminal = sway.terminal(config, '--debug-socket', str(debug))
        if not wait_for(lambda: ask(debug, 'windows') is not None, 10):
            raise RuntimeError('the terminal never answered on its debug socket')
        # Sway tiles the second window to the right half of the output.
        other_debug = sway.work / 'other.sock'
        sway.terminal(config, '--debug-socket', str(other_debug))
        if not wait_for(lambda: ask(other_debug, 'windows') is not None, 10):
            raise RuntimeError('the second terminal never answered on its debug socket')
        time.sleep(0.3)

        pointer.send('200 500', 'rpress', 'rrelease')
        if not wait_for(lambda: menu_open(debug), 5):
            raise RuntimeError('a right click opened no menu popup')

        # A click on the other client breaks the menu's grab.
        pointer.send('800 500', 'press', 'release')
        time.sleep(0.5)
        if terminal.poll() is not None:
            raise AssertionError('FAIL: the terminal exited when the compositor '
                                 'dismissed its menu')
        if ask(debug, 'windows') is None:
            raise AssertionError('FAIL: the terminal stopped answering after the '
                                 'compositor dismissed its menu')
        if not wait_for(lambda: not menu_open(debug), 3):
            raise AssertionError('FAIL: the dismissed menu is still open')
        print('PASS: the dismissed menu closed and its window stayed')


if __name__ == '__main__':
    main()
