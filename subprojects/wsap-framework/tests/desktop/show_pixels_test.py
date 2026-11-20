"""Display the example under headless Sway and close its native window."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def wait_for(check):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise RuntimeError('timed out waiting for the compositor or example')


def stop(process):
    if process.poll() is None:
        process.kill()
        process.wait(timeout=5)


def main(example, sway, swaymsg, grim):
    if os.getuid() == 0:
        print('SKIP: Sway does not run as root')
        return 77
    with tempfile.TemporaryDirectory(prefix='wsap-desktop-test-') as tmp:
        runtime = Path(tmp)
        env = dict(os.environ, XDG_RUNTIME_DIR=tmp, WLR_BACKENDS='headless',
                   WLR_RENDERER='pixman', WLR_LIBINPUT_NO_DEVICES='1',
                   WSAP_RENDER_GPU='0')
        env.pop('WAYLAND_DISPLAY', None)
        env.pop('DISPLAY', None)
        with (runtime / 'sway.log').open('w+') as log:
            compositor = subprocess.Popen([sway, '--unsupported-gpu', '-c', '/dev/null'],
                                          env=env, stdout=log, stderr=log)
            try:
                def socket(pattern):
                    if compositor.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    return next((p for p in runtime.glob(pattern) if p.is_socket()), None)

                ipc = wait_for(lambda: socket('sway-ipc.*.sock'))
                env['WAYLAND_DISPLAY'] = wait_for(lambda: socket('wayland-*')).name
                app = subprocess.Popen([example], env=env)
                try:
                    def painted():
                        if app.poll() is not None:
                            raise RuntimeError(f'example exited early: {app.returncode}')
                        ppm = subprocess.check_output([grim, '-t', 'ppm', '-'],
                                                      env=env, timeout=5)
                        _, _, _, pixels = ppm.split(b'\n', 3)
                        return b'\x42\xc6\xa5' * 16 in pixels and b'\x25\x28\x3d' * 16 in pixels

                    wait_for(painted)
                    reply = subprocess.check_output(
                        [swaymsg, '-s', str(ipc), '[title="^WSAP Pixel Example$"] kill'],
                        env=env, timeout=5)
                    assert json.loads(reply)[0]['success']
                    assert app.wait(timeout=5) == 0
                finally:
                    stop(app)
            finally:
                stop(compositor)
    return 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
