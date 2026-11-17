"""Check that a native menu popup takes keyboard rows and closes cleanly.

A key that activates a popup row can close the menu from inside the popup's
own dispatch. The dispatch still reads the popup surface after its busy
section, so the popup storage must stay alive until the window reaps it. The
terminal must stay alive, and the menu must close, for each case:

  - a mnemonic opens the Themes submenu, then Escape closes a level while a
    theme preview restyles the window;
  - a mnemonic activates a row that opens another window.
"""
import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

# The pointer extent equals the output size of headless Sway.
OUTPUT = ('1280', '720')
# Output pixels of the right click.
CLICK = (202, 327)

def ask(path, line):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(3)
            s.connect(str(path))
            s.sendall(line.encode() + b'\n')
            reply = s.makefile('rb').readline()
    except OSError:
        return None
    return json.loads(reply) if reply else None

def state(path):
    reply = ask(path, 'windows')
    return reply['result'][0] if reply and reply.get('ok') else None

def popups(path):
    return (state(path) or {}).get('menu_popups')

def open_menu(sway, pointer, debug):
    pointer.send(f'{CLICK[0]} {CLICK[1]}', 'rpress', 'rrelease')
    if not wait_for(lambda: popups(debug) == 1, 5):
        raise RuntimeError('a right click opened no menu popup')
    time.sleep(0.3)

def expect_closed(terminal, debug, name):
    if not wait_for(lambda: popups(debug) == 0, 5):
        raise AssertionError(f'FAIL: the menu stayed open after {name}')
    if terminal.poll() is not None:
        raise AssertionError(f'FAIL: the terminal died with status '
                             f'{terminal.poll()} after {name}')
    if state(debug) is None:
        raise AssertionError(f'FAIL: the terminal stopped answering after {name}')
    now = state(debug)
    if now['menu_open'] or now['menu_levels']:
        raise AssertionError(f'FAIL: the menu stayed open after {name}: {now}')

def press(debug, *chords):
    for chord in chords:
        reply = ask(debug, f'key {chord}')
        if not reply or not reply.get('ok'):
            raise RuntimeError(f'the terminal rejected the key {chord}')
        time.sleep(0.25)

def main():
    require('sway')
    with HeadlessSway() as sway:
        pointer = sway.device('pointer', *OUTPUT)
        debug = sway.work / 'debug.sock'
        config = ['command = /bin/cat', 'width = 800', 'height = 600']
        terminal = sway.terminal(config, '--debug-socket', str(debug))
        if not wait_for(lambda: state(debug) is not None, 10):
            raise RuntimeError('the terminal never answered on its debug socket')
        time.sleep(0.5)

        # A mnemonic opens the Themes submenu. A row then previews a theme,
        # which restyles the window, and Escape closes the level.
        open_menu(sway, pointer, debug)
        press(debug, 't', 'down', 'escape')
        if not wait_for(lambda: popups(debug) <= 1, 5):
            raise AssertionError('FAIL: Escape kept the submenu popup')
        press(debug, 'escape')
        expect_closed(terminal, debug, 'the theme preview and Escape')
        print('ok: keyboard rows and Escape close a previewed submenu')

        # A mnemonic activates a row that opens another window.
        open_menu(sway, pointer, debug)
        press(debug, 's')
        if not wait_for(lambda: popups(debug) == 0, 5):
            raise AssertionError('FAIL: a mnemonic left the menu open')
        if terminal.poll() is not None:
            raise AssertionError(f'FAIL: the terminal died with status '
                                 f'{terminal.poll()} after a mnemonic row')
        print('ok: a mnemonic row closes the menu and the terminal stays')

    print('PASS: keyboard rows close the menu and the terminal stays')

if __name__ == '__main__':
    main()
