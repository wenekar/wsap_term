"""A window draws when it must and not more. An animation draws at most one
frame per display refresh, and the window stops drawing and waiting when it
ends. Opening a tab starts the tab bar's animation, so a tab opened every
100 ms keeps it running. A blinking cursor draws one frame per blink while
the window is otherwise idle. Runs a private headless Sway (60 Hz) and reads
frame counts through the inspector socket."""
import json
import os
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for  # noqa: E402

REFRESH_HZ = 60
BLINK_HALF_S = 0.5
TABS = 10
TAB_EVERY_S = 0.1
# The slide animation of the last tab.
TAIL_S = 0.25


def ask(path, line):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(str(path))
        s.sendall(line.encode() + b'\n')
        return json.loads(s.makefile('rb').readline())


def frames(path):
    return ask(path, 'windows')['result'][0]['frame']


def answers(path):
    try:
        return ask(path, 'windows').get('ok', False)
    except OSError:
        return False


def cpu_ticks(pid):
    """User plus system time of the process, in clock ticks."""
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return int(fields[11]) + int(fields[12])


def main():
    require('sway')
    with HeadlessSway() as sway:
        debug = sway.work / 'debug.sock'
        terminal = sway.terminal(['command = /bin/cat', 'width = 800', 'height = 600',
                                  'cursor-blink = false'], '--debug-socket', str(debug))
        if not wait_for(lambda: answers(debug), 10):
            raise RuntimeError('the terminal never answered on its debug socket')
        time.sleep(0.5)

        before = frames(debug)
        start = time.monotonic()
        for i in range(TABS):
            assert ask(debug, 'shortcut new_tab')['ok']
            time.sleep(max(0.0, start + (i + 1) * TAB_EVERY_S - time.monotonic()))
        time.sleep(TAIL_S)
        animated = time.monotonic() - start
        during = frames(debug) - before
        # One frame per refresh, one more for each opened tab, and slack.
        limit = int(REFRESH_HZ * animated) + TABS + 5
        if during > limit:
            raise AssertionError(f'FAIL: {during} frames in {animated:.2f} s of animation '
                                 f'(limit {limit}); a frame must wait for the one '
                                 'before it to be shown')

        time.sleep(0.2)
        settled = frames(debug)
        ticks = cpu_ticks(terminal.pid)
        time.sleep(0.5)
        idle = frames(debug) - settled
        busy = cpu_ticks(terminal.pid) - ticks
        if idle > 2:
            raise AssertionError(f'FAIL: {idle} frames in 0.5 s after the animation '
                                 'ended; an idle window must not draw')
        # 5 ticks are 50 ms at the usual 100 Hz clock: far below a busy wait.
        if busy > 5:
            raise AssertionError(f'FAIL: {busy} CPU ticks in 0.5 s while idle '
                                 f'(clock {os.sysconf("SC_CLK_TCK")} Hz); the loop '
                                 'must sleep')

        # The new window takes the keyboard focus, so its cursor blinks.
        blink_debug = sway.work / 'blink.sock'
        sway.terminal(['command = /bin/cat', 'width = 800', 'height = 600',
                       'cursor-blink = true'], '--debug-socket', str(blink_debug))
        if not wait_for(lambda: answers(blink_debug), 10):
            raise RuntimeError('the blinking terminal never answered on its debug socket')
        time.sleep(0.5)
        start = frames(blink_debug)
        time.sleep(4 * BLINK_HALF_S)
        blinks = frames(blink_debug) - start
        if not 3 <= blinks <= 6:
            raise AssertionError(f'FAIL: {blinks} frames in {4 * BLINK_HALF_S:.1f} s of '
                                 'a blinking cursor; it must draw once per blink')
        print(f'PASS: {during} frames in {animated:.2f} s of animation (limit {limit}), '
              f'{idle} frames and {busy} CPU ticks while idle, {blinks} blink frames '
              f'in {4 * BLINK_HALF_S:.1f} s')


if __name__ == '__main__':
    main()
