"""Check that the window keeps its canvas between frames and never shows a
stale pixel. Runs a private headless Sway. The terminal logs its Wayland
requests, so the test sees the damage of each commit.

- An idle frame commits nothing. An idle frame with a popup menu open
  commits nothing on the window and nothing on the popup.
- A typed character damages one row band.
- A hover in a popup menu damages part of the popup.
- A resize damages the whole window.
- The alignment lint finds nothing on the main screens and on the popup.
- The same scenario runs again with WSAP_RENDER_FULL=1, which paints every
  frame fresh. At each step, the compositor output (grim) and the canvas
  of the two runs must match. The steps are typing, output, wheel
  scrolling with the scrollbar, a font size change, tabs, the command
  palette, a selection, a popup menu, and a resize."""
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

CONFIG = ['command = /bin/sh', 'width = 800', 'height = 600', 'cursor-blink = false']
XDG_SURFACE = re.compile(r'get_xdg_surface\(new id xdg_surface#\d+, wl_surface#(\d+)\)')
DAMAGE = re.compile(r'wl_surface#(\d+)\.damage_buffer\((-?\d+), (-?\d+), (\d+), (\d+)\)')
COMMIT = re.compile(r'wl_surface#(\d+)\.commit\(\)')
PPM = re.compile(rb'P6\s+(\d+)\s+(\d+)\s+255\s')

def ask(path, chain):
    """Sends each command of a ' ; ' chain on one connection, in order."""
    result = None
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect(str(path))
        reader = s.makefile('rb')
        for line in chain.split(' ; '):
            s.sendall(line.encode() + b'\n')
            reply = json.loads(reader.readline())
            if not reply.get('ok'):
                raise RuntimeError(f'{line!r} failed: {reply}')
            result = reply['result']
    return result

def answers(path):
    try:
        ask(path, 'windows')
        return True
    except (OSError, ValueError, RuntimeError):
        return False

class Log:
    """The Wayland requests of one terminal, read from a mark on."""

    def __init__(self, path):
        self.path = path
        self.at = 0

    def lines(self):
        return self.path.read_text(errors='replace').splitlines()

    def surfaces(self):
        """The wl_surface of each xdg_surface, oldest first."""
        return [m.group(1) for m in map(XDG_SURFACE.search, self.lines()) if m]

    def mark(self):
        self.at = len(self.lines())

    def entered(self, surface):
        """True when the pointer entered `surface` after the mark."""
        enter = re.compile(rf'wl_pointer#\d+\.enter\(\d+, wl_surface#{surface},')
        return any(enter.search(line) for line in self.lines()[self.at:])

    def since(self, surface):
        rects, commits = [], 0
        for line in self.lines()[self.at:]:
            m = DAMAGE.search(line)
            if m and m.group(1) == surface:
                rects.append(tuple(int(v) for v in m.groups()[1:]))
            m = COMMIT.search(line)
            if m and m.group(1) == surface:
                commits += 1
        return rects, commits

def covers(rects, size):
    return any(x <= 0 and y <= 0 and x + w >= size[0] and y + h >= size[1]
               for x, y, w, h in rects)

class Image:
    """RGB pixels, kept compressed."""

    def __init__(self, width, height, rgb):
        self.size = (width, height)
        self.packed = zlib.compress(rgb, 1)

    @classmethod
    def from_ppm(cls, data):
        m = PPM.match(data)
        if not m:
            raise RuntimeError('grim wrote no binary PPM')
        width, height = int(m.group(1)), int(m.group(2))
        return cls(width, height, data[m.end():m.end() + width * height * 3])

    @classmethod
    def from_png(cls, path):
        """Reads a PNG of the inspector: 8-bit RGB with filter 0 on each row."""
        data, at, idat = path.read_bytes(), 8, b''
        path.unlink()
        while at < len(data):
            n, kind = struct.unpack('>I4s', data[at:at + 8])
            body = data[at + 8:at + 8 + n]
            if kind == b'IHDR':
                width, height, depth, color = struct.unpack('>IIBB', body[:10])
                if (depth, color) != (8, 2):
                    raise RuntimeError(f'{path.name} is not 8-bit RGB')
            elif kind == b'IDAT':
                idat += body
            at += 12 + n
        raw, row = zlib.decompress(idat), width * 3 + 1
        if any(raw[y * row] for y in range(height)):
            raise RuntimeError(f'{path.name} has a row filter')
        return cls(width, height, b''.join(raw[y * row + 1:(y + 1) * row]
                                           for y in range(height)))

    def rgb(self):
        return zlib.decompress(self.packed)

    def difference(self, other):
        """None when the images are equal, else where they differ."""
        if self.size != other.size:
            return f'size {self.size} is not {other.size}'
        a, b = self.rgb(), other.rgb()
        if a == b:
            return None
        row = self.size[0] * 3
        xs, ys = [], []
        for y in range(self.size[1]):
            ra, rb = a[y * row:(y + 1) * row], b[y * row:(y + 1) * row]
            if ra != rb:
                xs += [x for x in range(self.size[0]) if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]]
                ys.append(y)
        return f'{len(xs)} pixels in x {min(xs)}..{max(xs)}, y {min(ys)}..{max(ys)}'

def settle(debug, seconds=0.3):
    ask(debug, 'frame')
    time.sleep(seconds)
    ask(debug, 'frame')

def window_state(debug):
    return ask(debug, 'windows')[0]

def size_of(debug):
    return tuple(window_state(debug)['size'])

def popup_open(debug):
    return window_state(debug)['menu_popups'] > 0

def scenario(sway, full, check):
    env = dict(sway.env, WAYLAND_DEBUG='client', PS1='$ ', ENV='/dev/null')
    if full:
        env['WSAP_RENDER_FULL'] = '1'
    work = sway.work
    debug = work / 'debug.sock'
    log = Log(work / 'wayland.log')
    with log.path.open('w') as err:
        sway.terminal(CONFIG, '--debug-socket', str(debug), env=env, stderr=err)
    if not wait_for(lambda: answers(debug), 10):
        raise RuntimeError('the terminal never answered on its debug socket')
    settle(debug, 0.5)
    surface = log.surfaces()[0]
    shots = []

    def grim():
        out = subprocess.run(['grim', '-t', 'ppm', '-'], env=sway.env, check=True,
                             stdout=subprocess.PIPE, timeout=10).stdout
        return Image.from_ppm(out)

    def shot(name, popup=False):
        """Saves the canvas, the popup canvas, and the compositor output."""
        settle(debug)
        path = work / f'{name}.png'
        ask(debug, f'screenshot {path}')
        shots.append((f'{name} canvas', Image.from_png(path)))
        if popup:
            ask(debug, f'-p 0 screenshot {path}')
            shots.append((f'{name} popup canvas', Image.from_png(path)))
        shots.append((f'{name} screen', grim()))

    def lint(where, option=''):
        if full:
            return
        found = ask(debug, f'{option}lint')
        check(found['count'] == 0, f'the lint found {found["findings"]} on {where}')

    def idle(what, surfaces):
        """Asks for three frames. `surfaces` must commit nothing."""
        log.mark()
        before = window_state(debug)['frame']
        for _ in range(3):
            ask(debug, 'frame')
        time.sleep(0.3)
        drawn = window_state(debug)['frame'] - before
        check(drawn >= 3, f'{what}: {drawn} frames were drawn for 3 frame requests')
        for name, wl_surface in surfaces:
            rects, commits = log.since(wl_surface)
            check(not rects and not commits,
                  f'{what}: the {name} committed {commits} times with damage {rects}')

    # Each yasl buffer gets full damage on its first commit, so warm them up.
    for _ in range(6):
        ask(debug, 'text a ; frame')
        time.sleep(0.05)
    ask(debug, 'key ctrl+u')
    settle(debug)
    if not full:
        idle('idle frames', [('window', surface)])

    log.mark()
    ask(debug, 'text x ; frame')
    time.sleep(0.3)
    width, height = size_of(debug)
    rects, _ = log.since(surface)
    bands = {(y, h) for _, y, _, h in rects}
    check(len(bands) == 1, f'typing damaged {rects}, not one row band')
    if len(bands) == 1:
        y, h = bands.pop()
        check(0 < h < height // 8, f'the typing band {y},{h} is not one row of {height}')
    shot('typed')
    lint('the terminal')

    ask(debug, 'key ctrl+u')
    ask(debug, 'text "seq 1 300" ; key enter')
    shot('output')

    # The wheel scrolls into the history and shows the scrollbar over the
    # grid. The pointer on the bar keeps it shown. When the pointer leaves,
    # the bar hides, and the grid under it must come back.
    ask(debug, f'scroll {width // 2} {height // 2} 5 ; move {width - 3} {height // 2}')
    shot('scrolled')
    parts = [p['name'] for p in ask(debug, 'widget scrollview')['parts']]
    check('scroll-thumb' in parts, f'the wheel showed no scrollbar: {parts}')
    ask(debug, f'move {width // 2} {height // 2} ; scroll {width // 2} {height // 2} -5')
    time.sleep(1.2)
    shot('scrolled-back')

    ask(debug, 'shortcut font_larger')
    shot('font-larger')
    lint('the terminal with a larger font')
    ask(debug, 'shortcut font_smaller')
    shot('font-smaller')

    ask(debug, 'shortcut new_tab')
    time.sleep(0.5)
    ask(debug, 'text "echo second tab" ; key enter')
    shot('second-tab')
    lint('the tab bar')
    ask(debug, 'shortcut prev_tab')
    shot('first-tab')

    ask(debug, 'shortcut command_palette')
    shot('palette')
    lint('the command palette')
    ask(debug, 'text tab')
    shot('palette-filter')
    ask(debug, 'key escape')
    shot('palette-closed')

    ask(debug, f'down 40 {height // 2} ; move {width // 2} {height // 2 + 40} ; '
               f'up {width // 2} {height // 2 + 40}')
    shot('selection')

    # A native popup needs a seat, so a virtual pointer opens it. The
    # pointer maps one to one to the output. The terminal binds the pointer
    # after the seat gets it, so the click waits for the pointer to enter.
    out = grim().size
    log.mark()
    pointer = sway.device('pointer', str(out[0]), str(out[1]))
    click = (out[0] // 3, out[1] // 3)
    pointer.send(f'{click[0]} {click[1]}')
    if not wait_for(lambda: log.entered(surface), 5):
        raise RuntimeError('the pointer did not enter the window')
    pointer.send('rpress', 'rrelease')
    if not wait_for(lambda: popup_open(debug), 5):
        raise RuntimeError('a right click opened no popup menu')
    popup = log.surfaces()[-1]
    if popup == surface:
        raise RuntimeError('the popup menu has no surface of its own')
    # Sway gives the pointer to the popup only when the popup is on screen
    # before the pointer moves onto it.
    settle(debug)
    log.mark()
    probe = (click[0] + 40, click[1] + 30)
    pointer.send(f'{probe[0]} {probe[1]}')
    if not wait_for(lambda: log.entered(popup), 5):
        raise RuntimeError('the pointer did not enter the popup menu')
    settle(debug)
    at = ask(debug, '-p 0 state')['pointer']['pos']
    origin = (probe[0] - at[0], probe[1] - at[1])
    items = {run['text']: run['box'] for run in ask(debug, '-p 0 widget /0/0')['text']}
    popup_size = tuple(ask(debug, '-p 0 tree --depth 1')['bounds'][2:])

    def hover(item):
        x, y, w, h = items[item]
        pointer.send(f'{origin[0] + x + w // 2} {origin[1] + y + h // 2}')

    shot('menu', popup=True)
    lint('the popup menu', '-p 0 ')
    if not full:
        idle('idle frames with a popup menu', [('window', surface), ('popup', popup)])
    # Warm up the buffers of the popup, as for the window.
    for i in range(6):
        hover(('Paste', 'Select All')[i % 2])
        ask(debug, 'frame')
        time.sleep(0.05)
    settle(debug)
    log.mark()
    hover('Paste')
    settle(debug)
    rects, commits = log.since(popup)
    check(commits and rects and not covers(rects, popup_size),
          f'a hover damaged {rects} in {commits} commits of a '
          f'{popup_size[0]}x{popup_size[1]} popup, not a part of it')
    shot('menu-hover', popup=True)
    ask(debug, 'key escape')
    if not wait_for(lambda: not popup_open(debug), 5):
        raise RuntimeError('escape did not close the popup menu')
    shot('menu-closed')

    log.mark()
    other = work / 'other.sock'
    sway.terminal(CONFIG, '--debug-socket', str(other), env=dict(sway.env, PS1='$ '))
    if not wait_for(lambda: answers(other), 10):
        raise RuntimeError('the second terminal never answered on its debug socket')
    if not wait_for(lambda: size_of(debug) != (width, height), 5):
        raise RuntimeError('the second window did not resize the first one')
    settle(debug)
    width, height = size_of(debug)
    rects, _ = log.since(surface)
    check(covers(rects, (width, height)),
          f'the resize to {width}x{height} damaged only {rects}')
    shot('resized')
    return shots

def main():
    require('sway', 'grim')
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    results = {}
    for full in (False, True):
        with HeadlessSway() as sway:
            results[full] = scenario(sway, full, check if not full else lambda ok, what: None)
    if [n for n, _ in results[False]] != [n for n, _ in results[True]]:
        raise RuntimeError('the two runs took different screenshots')
    for (name, mine), (_, fresh) in zip(results[False], results[True]):
        diff = mine.difference(fresh)
        check(diff is None, f'the {name} differs from a fresh paint: {diff}')
    if failures:
        raise AssertionError('FAIL: ' + '; '.join(failures))
    print(f'PASS: idle frames commit nothing, also with a popup menu open; typing '
          f'damages one row band; a hover damages part of the popup; a resize '
          f'damages everything; the lint finds nothing; and {len(results[False])} '
          'screenshots match a fresh paint')

if __name__ == '__main__':
    main()
