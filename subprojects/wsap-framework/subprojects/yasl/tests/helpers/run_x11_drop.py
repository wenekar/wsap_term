#!/usr/bin/env python3
"""Run the X11 drop test on Xwayland under a private headless weston.

Exit 77 (skip) when weston or Xwayland is not installed."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77


def wait_for(process, ready, what):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'{what} stopped before it was ready')
        if ready():
            return
        time.sleep(0.02)
    raise TimeoutError(f'{what} readiness')


def main():
    (test,) = sys.argv[1:]
    weston = shutil.which('weston')
    xwayland = shutil.which('Xwayland')
    if not weston or not xwayland:
        print('SKIP: needs weston and Xwayland')
        raise SystemExit(SKIP)
    servers = []
    code = 1
    with tempfile.TemporaryDirectory(prefix='yasl-drop-x11-') as directory:
        root = Path(directory)
        socket = 'yasl-drop-x11'
        env = os.environ.copy()
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET'):
            env.pop(name, None)
        env.update(XDG_RUNTIME_DIR=directory, XDG_SESSION_TYPE='wayland',
                   WAYLAND_DISPLAY=socket)
        with (root / 'weston.log').open('w+') as wl_log, (root / 'xwayland.log').open('w+') as x_log:
            try:
                wl = subprocess.Popen([weston, '--backend=headless', '--renderer=pixman',
                    f'--socket={socket}', '--idle-time=0', '--no-config',
                    '--width=1024', '--height=768'], env=env, stdout=wl_log, stderr=wl_log)
                servers.append(wl)
                wait_for(wl, lambda: (root / socket).is_socket(), 'weston')
                display_file = root / 'display'
                with display_file.open('w') as fd:
                    x = subprocess.Popen([xwayland, '-displayfd', str(fd.fileno()),
                        '-shm', '-noreset', '-nolisten', 'tcp'], env=env,
                        pass_fds=(fd.fileno(),), stdout=x_log, stderr=x_log)
                servers.append(x)
                wait_for(x, lambda: bool(display_file.read_text().strip()), 'Xwayland')
                env['DISPLAY'] = ':' + display_file.read_text().strip()
                env.pop('WAYLAND_DISPLAY', None)
                code = subprocess.run([test], env=env, timeout=240).returncode
            except BaseException:
                for log in (wl_log, x_log):
                    log.flush()
                    log.seek(0)
                    print(log.read(), file=sys.stderr)
                raise
            finally:
                for server in reversed(servers):
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
    if code:
        raise SystemExit(code)


if __name__ == '__main__':
    main()
