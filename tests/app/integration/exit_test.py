import json
from pathlib import Path
import signal
import socket
import subprocess
import sys

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

def check(terminal, how):
    try:
        out, _ = terminal.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        raise AssertionError(f'FAIL: the terminal still runs 5 s after {how}')
    if terminal.returncode != 0:
        raise AssertionError(f'FAIL: the terminal exited with {terminal.returncode} '
                             f'after {how}')
    if out:
        raise AssertionError(f'FAIL: the terminal wrote {out[:120]!r} to stdout after '
                             f'{how}; with stats off it must write nothing')

def main():
    require('sway')
    with HeadlessSway() as sway:
        config = ['command = /bin/cat', 'width = 800', 'height = 600']
        for case, how in (('close', 'its last tab closed'), ('term', 'SIGTERM')):
            debug = sway.work / f'{case}.sock'
            terminal = sway.terminal(config, '--debug-socket', str(debug),
                                     stdout=subprocess.PIPE)
            if not wait_for(lambda: ask(debug, 'windows') is not None, 10):
                raise RuntimeError('the terminal never answered on its debug socket')
            if case == 'close':
                ask(debug, 'shortcut close_tab')
            else:
                terminal.send_signal(signal.SIGTERM)
            check(terminal, how)
        print('PASS: the terminal exits with status 0 and a clean stdout')

if __name__ == '__main__':
    main()
