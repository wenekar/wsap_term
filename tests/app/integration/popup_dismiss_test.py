import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

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

def menu_open(path):
    reply = ask(path, 'tree -p 0 --depth 1')
    return bool(reply and reply.get('ok'))

def main():
    require('sway')
    with HeadlessSway() as sway:
        pointer = sway.device('pointer', '1000', '1000')
        debug = sway.work / 'debug.sock'
        config = ['command = /bin/cat', 'width = 800', 'height = 600']
        terminal = sway.terminal(config, '--debug-socket', str(debug))
        if not wait_for(lambda: ask(debug, 'windows') is not None, 10):
            raise RuntimeError('the terminal never answered on its debug socket')
        other_debug = sway.work / 'other.sock'
        sway.terminal(config, '--debug-socket', str(other_debug))
        if not wait_for(lambda: ask(other_debug, 'windows') is not None, 10):
            raise RuntimeError('the second terminal never answered on its debug socket')
        time.sleep(0.3)

        pointer.send('200 500', 'rpress', 'rrelease')
        if not wait_for(lambda: menu_open(debug), 5):
            raise RuntimeError('a right click opened no menu popup')

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
