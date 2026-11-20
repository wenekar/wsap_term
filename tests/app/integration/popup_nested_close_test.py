"""Check that a menu closes cleanly when the pointer presses a popup's shadow
during a theme preview. The preview restyles the menu, and the restyle lays
out the menu again. That layout can close popups while the pressed popup
still dispatches the press. The test presses the shadow of each level in
turn. Each press runs in its own private headless Sway. The terminal must
stay alive, and the menu must close."""
import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

# The pointer extent equals the output size of headless Sway.
OUTPUT = ('1280', '720')
# Output pixels of the right click, and the offsets of the Themes row from it.
CLICK = (202, 327)
THEMES_ROW = 88
ROW_X = 60
# A point in the shadow margin of a popup. The menu lists start further in.
SHADOW = (3, 3)
# Popup levels of the debug socket, 0-based. Level 0 is the context menu.
# Level 1 is the Themes submenu.
LEVELS = {'context menu': 0, 'Themes submenu': 1}

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

def submenu_background(path):
    reply = ask(path, 'widget -p 1 /0/0')
    if not reply or not reply.get('ok'):
        return None
    return reply['result']['style']['background']

def press_shadow(name, level):
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

        # Down twice in the submenu highlights a theme, which previews it.
        before = submenu_background(debug)
        for _ in range(2):
            ask(debug, 'key down')
            time.sleep(0.2)
        if before is None or submenu_background(debug) == before:
            raise RuntimeError('the theme preview did not restyle the submenu')

        ask(debug, f'click {SHADOW[0]} {SHADOW[1]} left -p {level}')
        time.sleep(0.5)
        if terminal.poll() is not None:
            raise AssertionError(f'FAIL: the terminal died with status {terminal.poll()} '
                                 f'when the {name} shadow was pressed during the preview')
        now = state(debug)
        if now is None:
            raise AssertionError(f'FAIL: the terminal stopped answering after the {name} '
                                 'shadow was pressed during the preview')
        if (now['menu_open'], now['menu_levels'], now['menu_popups']) != (False, 0, 0):
            raise AssertionError('FAIL: the menu did not close after the '
                                 f'{name} shadow was pressed: {now["menu_levels"]} levels, '
                                 f'{now["menu_popups"]} popups')

def main():
    require('sway')
    for name, level in LEVELS.items():
        press_shadow(name, level)
    print('PASS: the menu closed and the terminal stayed for each popup shadow')

if __name__ == '__main__':
    main()
