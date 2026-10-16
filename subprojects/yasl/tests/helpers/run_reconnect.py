#!/usr/bin/env python3
"""Kill Weston under a running client, start a new one on the same socket, and
require the client to reconnect: run_reconnect.py <weston> <client>."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def start_weston(weston, root, env, log):
    process = subprocess.Popen([weston, '--backend=headless', '--renderer=gl', '--socket=yasl-test',
                                '--idle-time=0', '--no-config', '--width=1024', '--height=768'],
                               env=env, stdout=log, stderr=log)
    deadline = time.monotonic() + 12
    while not (root/'yasl-test').is_socket():
        if process.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError('Weston did not start')
        time.sleep(0.02)
    return process


def expect(client, word):
    line = client.stdout.readline().strip()
    if line != word:
        raise RuntimeError(f'client printed {line!r}, expected {word!r}')


def main():
    weston, binary = sys.argv[1:]
    signal.alarm(50)
    with tempfile.TemporaryDirectory(prefix='yasl-reconnect-') as directory:
        root = Path(directory)
        env = os.environ.copy()
        env.pop('WAYLAND_SOCKET', None)
        env.update(XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY='yasl-test', LIBGL_ALWAYS_SOFTWARE='1')
        lavapipe = next(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'), None)
        if lavapipe:
            env['VK_DRIVER_FILES'] = str(lavapipe)
        with (root/'weston.log').open('w+') as log:
            servers = [start_weston(weston, root, env, log)]
            client = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE, text=True)
            try:
                expect(client, 'ready')
                servers[0].send_signal(signal.SIGKILL)
                servers[0].wait()
                expect(client, 'lost')
                time.sleep(0.5)
                servers.append(start_weston(weston, root, env, log))
                expect(client, 'restored')
                if client.wait(timeout=15) != 0:
                    raise RuntimeError(f'client exited with {client.returncode}')
            except BaseException:
                client.kill()
                log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
                raise
            finally:
                for server in servers:
                    if server.poll() is None:
                        server.terminate()
                        server.wait(timeout=5)


if __name__ == '__main__':
    main()
