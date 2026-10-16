"""Check that the display's own timers and pipes wake the terminal while it
is otherwise idle: a held key repeats, and a large paste arrives in full.
Runs a private headless Sway with a virtual keyboard. Usage:
display_wake_test.py repeat|paste"""
import os
from pathlib import Path
import subprocess
import sys
import time
import tty

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

KEY_A, KEY_B, KEY_LEFTSHIFT, KEY_INSERT = 30, 48, 42, 110
PASTE_BYTES = 192 * 1024

def child(log, ready):
    tty.setraw(0)
    Path(ready).touch()
    with open(log, 'a') as out:
        while True:
            try:
                data = os.read(0, 1 << 16)
            except OSError:
                return 0
            if not data:
                return 0
            out.write(f'{time.monotonic():.6f} {data.count(b"a")} '
                      f'{data.count(b"b")} {data.count(b"x")}\n')
            out.flush()

def reads(log):
    if not log.exists():
        return []
    rows = []
    for line in log.read_text().splitlines():
        t, a, b, x = line.split()
        rows.append((float(t), int(a), int(b), int(x)))
    return rows

def repeat(keyboard, log):
    keyboard.send(f'press {KEY_A}')
    pressed = time.monotonic()
    time.sleep(1.5)
    released = time.monotonic()
    keyboard.send(f'release {KEY_A}')
    time.sleep(0.5)
    held = sum(a for t, a, _, _ in reads(log) if t < released)
    total = sum(a for _, a, _, _ in reads(log))
    if held < 10:
        raise AssertionError(f'FAIL: {held} characters arrived while the key was held '
                             f'{released - pressed:.2f} s ({total} after release); '
                             'the repeat timer did not wake the terminal')
    print(f'PASS: {held} characters while held {released - pressed:.2f} s')

def paste(sway, keyboard, log):
    copier = sway.spawn(['wl-copy', '--foreground', '--type', 'text/plain'],
                        stdin=subprocess.PIPE)
    copier.stdin.write(b'x' * PASTE_BYTES)
    copier.stdin.close()
    offered = lambda: subprocess.run(['wl-paste', '--no-newline'], env=sway.env,
                                     capture_output=True, timeout=5).stdout == b'x' * PASTE_BYTES
    if not wait_for(offered, 5):
        raise RuntimeError('wl-copy never offered the selection')
    keyboard.send(f'press {KEY_LEFTSHIFT}', f'press {KEY_INSERT}',
                  f'release {KEY_INSERT}', f'release {KEY_LEFTSHIFT}')
    start = time.monotonic()
    arrived = lambda: sum(x for _, _, _, x in reads(log))
    if not wait_for(lambda: arrived() >= PASTE_BYTES, 3):
        stalled = arrived()
        taps = 0
        while taps < 100 and arrived() < PASTE_BYTES:
            keyboard.send(f'press {KEY_B}', f'release {KEY_B}')
            taps += 1
            time.sleep(0.02)
        raise AssertionError(f'FAIL: {stalled} of {PASTE_BYTES} pasted bytes arrived '
                             f'in 3 s, {arrived()} after {taps} unrelated key presses; '
                             'the clipboard pipe did not wake the terminal')
    print(f'PASS: {PASTE_BYTES} pasted bytes in {time.monotonic() - start:.2f} s')

def run(case):
    require('sway', *(['wl-copy', 'wl-paste'] if case == 'paste' else []))
    with HeadlessSway() as sway:
        keyboard = sway.device('keyboard')
        log, ready = sway.work / 'reads', sway.work / 'ready'
        sway.terminal([f'command = {sys.executable} {Path(__file__).resolve()} '
                       f'--child {log} {ready}',
                       'width = 800', 'height = 600', 'cursor-blink = false'])
        if not wait_for(ready.exists, 10):
            raise RuntimeError('the terminal never started its child')
        keyboard.send(f'press {KEY_B}', f'release {KEY_B}')
        if not wait_for(lambda: any(b for _, _, b, _ in reads(log)), 5):
            raise RuntimeError('a typed key never reached the terminal')
        if case == 'repeat':
            repeat(keyboard, log)
        else:
            paste(sway, keyboard, log)

if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--child':
        raise SystemExit(child(sys.argv[2], sys.argv[3]))
    if len(sys.argv) != 2 or sys.argv[1] not in ('repeat', 'paste'):
        raise SystemExit(__doc__)
    run(sys.argv[1])
