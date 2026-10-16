"""Check PTY response order while fastpipe is active, using a private headless Sway."""
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty

def write_all(fd, data):
    while data:
        n = os.write(fd, data)
        data = data[n:]

def child(report):
    tty.setraw(0)
    stop = threading.Event()
    # Volume of concurrent fast-pipe traffic, matching what the old in-band
    # termios markers used to contribute. The bytes are arbitrary; what this
    # test measures is PTY reply ordering under fast-pipe write pressure.
    noise_block = bytes(range(32, 127)) * 14

    def pipe_traffic():
        while not stop.is_set():
            write_all(1, noise_block)
            stop.wait(0.001)

    noise = threading.Thread(target=pipe_traffic, daemon=True)
    noise.start()
    expected = [f'\x1b[1;{1 + i % 20}R'.encode() for i in range(8000)]
    commands = b''.join(f'\x1b[1;{1 + i % 20}H\x1b[6n'.encode()
                        for i in range(len(expected)))
    tty_fd = os.open('/dev/tty', os.O_WRONLY)
    writer = threading.Thread(target=write_all, args=(tty_fd, commands), daemon=True)
    writer.start()
    received = bytearray()
    total = sum(map(len, expected))
    deadline = time.monotonic() + 15
    while len(received) < total and time.monotonic() < deadline:
        if select.select([0], [], [], 0.1)[0]:
            received.extend(os.read(0, 65536))
    stop.set()
    writer.join(timeout=1)
    noise.join(timeout=1)
    want = b''.join(expected)
    if received != want:
        responses = re.findall(rb'\x1b\[\d+;\d+R', received)
        mismatch = next((i for i, (a, b) in enumerate(zip(responses, expected))
                         if a != b), min(len(responses), len(expected)))
        Path(report).write_text(f'FAIL: {len(responses)} responses, first mismatch {mismatch}\n')
        return 1
    Path(report).write_text('PASS: 8000 cursor replies remain ordered during fastpipe traffic\n')
    return 0

def run():
    if not shutil.which('sway'):
        raise SystemExit('sway is required for the PTY ordering test')
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='clernen-pty-order-') as directory:
        work = Path(directory)
        env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_BACKENDS='headless',
                   WLR_LIBINPUT_NO_DEVICES='1', WLR_RENDERER='pixman',
                   WSAP_FAST_PIPE='1', WSAP_CONFIG='NONE')
        env.pop('LD_PRELOAD', None)
        delay = work / 'read-delay.so'
        subprocess.run(['cc', '-shared', '-fPIC', str(root / 'tests/pty/pty_read_delay.c'),
                        '-ldl', '-o', str(delay)], check=True)
        processes = []
        with (work / 'sway.log').open('w+') as sway_log, (work / 'term.log').open('w+') as term_log:
            try:
                sway = subprocess.Popen(['sway', '--unsupported-gpu', '-c', '/dev/null'],
                                        env=env, stdout=sway_log, stderr=sway_log)
                processes.append(sway)
                deadline = time.monotonic() + 5
                sockets = []
                while time.monotonic() < deadline and sway.poll() is None:
                    sockets = [p for p in work.glob('wayland-*') if p.is_socket()]
                    if sockets:
                        break
                    time.sleep(0.05)
                if not sockets:
                    sway_log.seek(0)
                    raise RuntimeError('headless Sway failed: ' + sway_log.read())
                env['WAYLAND_DISPLAY'] = sockets[0].name
                env['LD_PRELOAD'] = str(delay)
                report = work / 'result'
                config = work / 'term.conf'
                config.write_text(f'command = {sys.executable} {Path(__file__).resolve()} --child {report}\n'
                                  'width = 800\nheight = 600\n')
                terminal = subprocess.Popen([os.environ.get('BIN', str(root / 'build/bin/wsap_term')), '--config', str(config)],
                                            env=env, stdout=term_log, stderr=term_log)
                processes.append(terminal)
                terminal.wait(timeout=25)
                result = report.read_text() if report.exists() else 'FAIL: no child report'
                if terminal.returncode or not result.startswith('PASS:'):
                    term_log.seek(0)
                    raise AssertionError(result + '\n' + term_log.read())
                print(result, end='')
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--child':
        raise SystemExit(child(sys.argv[2]))
    run()
