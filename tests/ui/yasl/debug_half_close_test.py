import json
from pathlib import Path
import socket
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'

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

def half_closed(path, line):
    """Sends one command, ends the write side, and reads until the server closes."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect(str(path))
        s.sendall(line.encode() + b'\n')
        s.shutdown(socket.SHUT_WR)
        return s.makefile('rb').read()

def main():
    require('sway')
    with HeadlessSway() as sway:
        debug = sway.work / 'debug.sock'
        sway.terminal(['command = /bin/cat', 'width = 800', 'height = 600',
                       'cursor-blink = false'], '--debug-socket', str(debug))
        if not wait_for(lambda: ask(debug, 'windows') is not None, 10):
            raise RuntimeError('the terminal never answered on its debug socket')

        shot = sway.work / 'half-closed.png'
        raw = half_closed(debug, f'screenshot {shot}')
        if raw.count(b'\n') != 1 or not raw.endswith(b'\n'):
            raise AssertionError(f'FAIL: the half-closed screenshot must get one reply '
                                 f'line, got {raw[:120]!r}')
        reply = json.loads(raw)
        if not reply.get('ok') or reply['result']['path'] != str(shot):
            raise AssertionError(f'FAIL: the half-closed screenshot reply is {reply}')
        if not shot.read_bytes().startswith(PNG_SIGNATURE):
            raise AssertionError('FAIL: the half-closed screenshot is not a PNG file')
        print('PASS: a client that ends its writes still gets the screenshot reply')

if __name__ == '__main__':
    main()
