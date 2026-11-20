"""Check that a theme preview does not crash the terminal. Runs a private
headless Sway with a virtual pointer. The test opens the context menu and
its Themes submenu, highlights a theme, and moves the pointer back to the
first level. The preview ends then. The terminal must stay alive, and the
submenu popup must close."""
import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

# The pointer extent equals the output size of headless Sway.
OUTPUT = ('1280', '720')
# Output pixels of the right click, and the offsets of the menu rows from it.
CLICK = (202, 327)
THEMES_ROW = 88
SELECT_ALL_ROW = 58
ROW_X = 60

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

        pointer.send(f'{CLICK[0]} {CLICK[1]}', 'rpress', 'rrelease')
        if not wait_for(lambda: (state(debug) or {}).get('menu_popups') == 1, 5):
            raise RuntimeError('a right click opened no menu popup')
        # The popup may not have the pointer focus yet. Move again until
        # the hover delay opens the submenu.
        opened = False
        for step in range(10):
            pointer.send(f'{CLICK[0] + ROW_X + step % 2} {CLICK[1] + THEMES_ROW}')
            opened = wait_for(lambda: (state(debug) or {}).get('menu_popups') == 2, 1)
            if opened:
                break
        if not opened:
            raise RuntimeError('hovering Themes opened no submenu popup')
        # Down in the submenu highlights a theme, which previews it.
        for _ in range(2):
            ask(debug, 'key down')
            time.sleep(0.2)

        pointer.send(f'{CLICK[0] + ROW_X} {CLICK[1] + SELECT_ALL_ROW}')
        time.sleep(0.5)
        if terminal.poll() is not None:
            raise AssertionError(f'FAIL: the terminal died with status {terminal.poll()} '
                                 'when the theme preview ended')
        now = state(debug)
        if now is None:
            raise AssertionError('FAIL: the terminal stopped answering after the preview')
        if (now['menu_levels'], now['menu_popups']) != (1, 1):
            raise AssertionError('FAIL: the submenu did not close: '
                                 f'{now["menu_levels"]} levels, {now["menu_popups"]} popups')
        print('PASS: the preview ended, the submenu closed, and the terminal stayed')

if __name__ == '__main__':
    main()
