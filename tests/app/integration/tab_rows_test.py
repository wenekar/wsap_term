import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'helpers'))
from headless_sway import HeadlessSway, require, wait_for

def ask(sock, reader, line):
    sock.sendall(line.encode() + b'\n')
    reply = json.loads(reader.readline())
    if not reply.get('ok'):
        raise RuntimeError(f'{line!r} failed: {reply}')
    return reply['result']

def find(node, kind):
    if node['type'] == kind:
        return node
    for child in node.get('children', []):
        found = find(child, kind)
        if found:
            return found
    return None

def view_height(sock, reader):
    return find(ask(sock, reader, 'tree'), 'terminal-view')['bounds'][3]

def shell_rows(sock, reader, out, name):
    path = out / name
    ask(sock, reader, f'type stty size > {path}')
    ask(sock, reader, 'key Return')
    if not wait_for(lambda: path.exists() and path.read_text().endswith('\n'), 5):
        raise AssertionError(f'FAIL: the shell never wrote {name}')
    return int(path.read_text().split()[0])

def expect_view(sock, reader, bar):
    # Wait until the bar animation ends and the view keeps its height.
    last = []
    def settled():
        view = find(ask(sock, reader, 'tree'), 'terminal-view')['bounds']
        last.append(view)
        time.sleep(0.15)
        return (view[1] > 0) == bar and len(last) > 1 and last[-2] == view
    if not wait_for(settled, 5):
        raise AssertionError(f'FAIL: the tab bar should be {"shown" if bar else "hidden"}')
    return last[-1][3]

def main():
    require('sway')
    with HeadlessSway() as sway:
        debug = sway.work / 'debug.sock'
        terminal = sway.terminal(['command = /bin/sh', 'width = 800', 'height = 600'],
                                 '--debug-socket', str(debug))
        if not wait_for(lambda: debug.exists(), 10):
            raise RuntimeError('the terminal never opened its debug socket')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(5)
            sock.connect(str(debug))
            reader = sock.makefile('rb')
            ask(sock, reader, 'frame')
            h1 = view_height(sock, reader)
            rows1 = shell_rows(sock, reader, sway.work, 'one')

            ask(sock, reader, 'shortcut new_tab')
            h2 = expect_view(sock, reader, True)
            rows2 = shell_rows(sock, reader, sway.work, 'two')
            ask(sock, reader, 'shortcut close_tab')
            h3 = expect_view(sock, reader, False)
            rows3 = shell_rows(sock, reader, sway.work, 'three')

        # The cell height is unknown, so accept any one that fits all three views.
        fits = [c for c in range(4, 200)
                if h1 // c == rows1 and h2 // c == rows2 and h3 // c == rows3]
        if not fits:
            raise AssertionError(
                f'FAIL: the shell shows {rows1}, {rows2}, {rows3} rows for views '
                f'{h1}, {h2}, {h3} px high; no cell height fits')
        print(f'PASS: the shell rows follow the view ({rows1}, {rows2}, {rows3})')
        terminal.terminate()

if __name__ == '__main__':
    main()
