"""Check that an open popup menu follows a change of the output scale. Runs a
private headless Sway with a virtual pointer. The test opens the context
menu at scale 1, then sets the output scale to 1.5. The canvas of the popup
must then have the size of a popup that opens directly at scale 1.5, and it
must hold the whole menu."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

# The pointer extent equals the output size of headless Sway.
OUTPUT = ('1280', '720')
CLICK = '202 327'
SCALE = '1.5'

def ask(path, line):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            s.connect(str(path))
            s.sendall(line.encode() + b'\n')
            reply = s.makefile('rb').readline()
    except OSError:
        return None
    reply = json.loads(reply) if reply else None
    return reply['result'] if reply and reply.get('ok') else None

def popups(debug):
    state = ask(debug, 'windows')
    return state[0]['menu_popups'] if state else None

def open_menu(pointer, debug):
    pointer.send(CLICK, 'rpress', 'rrelease')
    if not wait_for(lambda: popups(debug) == 1, 5):
        raise RuntimeError('a right click opened no menu popup')

def popup_canvas(debug, path):
    """The size of the popup canvas and of its menu, with the shadow."""
    ask(debug, 'frame')
    shot = ask(debug, f'-p 0 screenshot {path}')
    tree = ask(debug, '-p 0 tree --depth 1')
    if not shot or not tree:
        raise RuntimeError('the popup answered no screenshot or tree')
    path.unlink()
    return tuple(shot['size']), tuple(tree['bounds'][2:])

def main():
    require('sway', 'swaymsg')
    with HeadlessSway() as sway:
        pointer = sway.device('pointer', *OUTPUT)
        debug = sway.work / 'debug.sock'
        config = ['command = /bin/cat', 'width = 800', 'height = 600']
        terminal = sway.terminal(config, '--debug-socket', str(debug))
        if not wait_for(lambda: ask(debug, 'windows') is not None, 10):
            raise RuntimeError('the terminal never answered on its debug socket')
        time.sleep(0.5)

        def swaymsg(*args):
            sockets = sorted(sway.work.glob('sway-ipc.*.sock'))
            return subprocess.run(['swaymsg', '-s', str(sockets[0]), *args], env=sway.env,
                                  check=True, stdout=subprocess.PIPE, timeout=10).stdout

        output = json.loads(swaymsg('-t', 'get_outputs', '-r'))[0]['name']
        shot = sway.work / 'popup.png'

        open_menu(pointer, debug)
        swaymsg('output', output, 'scale', SCALE)
        # The scale change reaches the terminal as a new window size.
        time.sleep(1.0)
        if terminal.poll() is not None:
            raise AssertionError('FAIL: the terminal exited on the scale change')
        if popups(debug) != 1:
            raise RuntimeError('the popup closed on the scale change')
        changed, content = popup_canvas(debug, shot)

        ask(debug, 'key escape')
        if not wait_for(lambda: popups(debug) == 0, 5):
            raise RuntimeError('the menu did not close')
        open_menu(pointer, debug)
        time.sleep(0.5)
        direct, _ = popup_canvas(debug, shot)

        print(f'canvas after the scale change {changed}, opened at the scale {direct}, '
              f'menu {content}')
        if changed != direct:
            raise AssertionError(f'FAIL: the popup canvas is {changed} after the scale '
                                 f'change, and {direct} when opened at that scale')
        if changed[0] < content[0] or changed[1] < content[1]:
            raise AssertionError(f'FAIL: the popup canvas {changed} cuts off the menu {content}')
        print('PASS: the popup canvas follows the output scale')

if __name__ == '__main__':
    main()
