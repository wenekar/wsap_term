"""Check that an open popup menu follows a change of the output scale. Runs a
private headless Sway with a virtual pointer. The test opens the context
menu at scale 1, then sets the output scale to 1.5. The canvas of the popup
must then have the size of a popup that opens directly at scale 1.5, and it
must hold the whole menu. A second part opens the menu at scales 1.5 and 2. The
canvas must then have the size of the menu, and the menu must open at the
pointer."""
import io
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

# The pointer extent equals the output size of headless Sway.
OUTPUT = ('1280', '720')
CLICK = '202 227'
SCALE = '1.5'
SCALES = ('1', '1.5', '2')
# The canvas can be one pixel larger than the menu, from rounding.
SLACK = 2
# The first visible pixel of the menu is the shadow width after the pointer.
MARGIN = 40
SHADOW_MAX = 30

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

def capture(sway):
    """The whole output, in physical pixels."""
    png = subprocess.run(['grim', '-t', 'png', '-'], env=sway.env, check=True,
                         stdout=subprocess.PIPE, timeout=10).stdout
    return Image.open(io.BytesIO(png)).convert('RGB')

def check_scale(sway, pointer, debug, swaymsg, output, scale, shot):
    """Opens the menu at a scale. Checks the canvas size and the position."""
    swaymsg('output', output, 'scale', scale)
    time.sleep(1.0)
    mode = json.loads(swaymsg('-t', 'get_outputs', '-r'))[0]['current_mode']
    before = capture(sway)
    open_menu(pointer, debug)
    time.sleep(0.5)
    canvas, menu = popup_canvas(debug, shot)
    after = capture(sway)
    # The pointer extent maps onto the output. The menu starts at the pointer.
    want = (round(int(CLICK.split()[0]) / int(OUTPUT[0]) * after.width),
            round(int(CLICK.split()[1]) / int(OUTPUT[1]) * after.height))
    # The terminal can redraw on the click. Look only near and after the pointer.
    area = (want[0] - MARGIN, want[1] - MARGIN, after.width, after.height)
    box = ImageChops.difference(before.crop(area), after.crop(area)).getbbox()
    if not box:
        raise RuntimeError('the menu changed no pixel near the pointer')
    box = (box[0] + area[0], box[1] + area[1], box[2] + area[0], box[3] + area[1])
    print(f'scale {scale}: canvas {canvas}, menu {menu}, changed pixels {box}, '
          f'pointer {want}, output {after.size}, mode {mode["width"]}x{mode["height"]}')
    if canvas[0] > menu[0] + SLACK or canvas[1] > menu[1] + SLACK:
        raise AssertionError(f'FAIL: at scale {scale} the popup canvas {canvas} is '
                             f'larger than the menu {menu}')
    if canvas[0] < menu[0] or canvas[1] < menu[1]:
        raise AssertionError(f'FAIL: at scale {scale} the popup canvas {canvas} cuts '
                             f'off the menu {menu}')
    if not (0 <= box[0] - want[0] <= SHADOW_MAX and 0 <= box[1] - want[1] <= SHADOW_MAX):
        raise AssertionError(f'FAIL: at scale {scale} the menu starts at {box[:2]}, '
                             f'and the pointer is at {want}')
    ask(debug, 'key escape')
    if not wait_for(lambda: popups(debug) == 0, 5):
        raise RuntimeError('the menu did not close')

def main():
    require('sway', 'swaymsg', 'grim')
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

        ask(debug, 'key escape')
        wait_for(lambda: popups(debug) == 0, 5)
        for scale in SCALES:
            check_scale(sway, pointer, debug, swaymsg, output, scale, shot)
        print('PASS: the popup opens at the pointer, with the size of the menu')

if __name__ == '__main__':
    main()
