#!/usr/bin/env python3
"""Run the public surface/GPU contract tests on private Weston and Xwayland."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

def wait_for(process, ready):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('Display server stopped before readiness')
        if ready():
            return
        time.sleep(0.02)
    raise TimeoutError('Display server readiness')

def main():
    # Optional trailing binaries run only in the X11 phase.
    weston, xwayland, contract, hello, tablet, gl, vulkan, notepad, *x11_only = sys.argv[1:]
    servers = []
    with tempfile.TemporaryDirectory(prefix='yasl-surfaces-') as directory:
        root = Path(directory)
        env = os.environ.copy()
        env.pop('WAYLAND_SOCKET', None)
        env.update(XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY='yasl-test',
                   XDG_SESSION_TYPE='wayland', LIBGL_ALWAYS_SOFTWARE='1')
        lavapipe = next(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json'), None)
        if lavapipe:
            env['VK_DRIVER_FILES'] = str(lavapipe)
        with (root/'weston.log').open('w+') as wl_log, (root/'xwayland.log').open('w+') as x_log:
            try:
                wl = subprocess.Popen([weston, '--backend=headless', '--renderer=gl',
                    '--socket=yasl-test', '--idle-time=0', '--no-config',
                    '--width=1024', '--height=768'], env=env, stdout=wl_log, stderr=wl_log)
                servers.append(wl)
                wait_for(wl, lambda: (root/'yasl-test').is_socket())
                display_file = root/'display'
                with display_file.open('w') as fd:
                    x = subprocess.Popen([xwayland, '-displayfd', str(fd.fileno()),
                        '-shm', '-noreset', '-nolisten', 'tcp'], env=env,
                        pass_fds=(fd.fileno(),), stdout=x_log, stderr=x_log)
                servers.append(x)
                wait_for(x, lambda: bool(display_file.read_text().strip()))
                env['DISPLAY'] = ':' + display_file.read_text().strip()
                for backend in ('wayland', 'x11'):
                    case = env.copy()
                    if backend == 'x11':
                        case.pop('WAYLAND_DISPLAY', None)
                        case['XDG_SESSION_TYPE'] = 'x11'
                    cases = [(contract,[backend]), (gl,[backend]),
                            (vulkan,[backend]), (tablet,['--backend',backend,'--smoke-test']),
                            (notepad,['--smoke-test'])]
                    if backend == 'x11':
                        cases += [(binary, []) for binary in x11_only]
                    for binary, args in cases:
                        if binary == '-':
                            continue
                        print('Testing', Path(binary).name, backend, flush=True)
                        subprocess.run([binary, *args], env=case, check=True, timeout=15)
                    # hello runs until closed: it must survive its first frames.
                    if hello == '-':
                        continue
                    print('Testing', Path(hello).name, backend, flush=True)
                    process = subprocess.Popen([hello], env=case)
                    try:
                        process.wait(timeout=1)
                        raise RuntimeError(f'{Path(hello).name} exited with {process.returncode}')
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        process.wait()
            except BaseException:
                for log in (wl_log,x_log):
                    log.flush();log.seek(0);print(log.read(), file=sys.stderr)
                raise
            finally:
                for server in reversed(servers):
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill();server.wait()
if __name__ == '__main__':
    main()
