import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]

def wait_for(predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()

def require(*tools):
    missing = [tool for tool in tools if not shutil.which(tool)]
    if missing:
        print('SKIP: needs ' + ', '.join(missing))
        raise SystemExit(77)

class Device:

    def __init__(self, args, env, log, processes):
        self.process = subprocess.Popen(args, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=log, text=True)
        processes.append(self.process)
        self.expect_ok()

    def expect_ok(self, timeout=5):
        ready, _, _ = select.select([self.process.stdout], [], [], timeout)
        line = self.process.stdout.readline() if ready else ''
        if line.strip() != 'ok':
            raise RuntimeError(f'{self.process.args[0]} failed: {line!r}')

    def send(self, *commands):
        for command in commands:
            self.process.stdin.write(command + '\n')
            self.process.stdin.flush()
            self.expect_ok()

class HeadlessSway:
    def __enter__(self):
        self.directory, self.log, self.processes = None, None, []
        try:
            return self._start()
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def _start(self):
        self.directory = tempfile.TemporaryDirectory(prefix='wsap-sway-')
        self.work = Path(self.directory.name)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.directory.name,
                        WLR_BACKENDS='headless', WLR_LIBINPUT_NO_DEVICES='1',
                        WLR_RENDERER='pixman', WSAP_CONFIG='NONE')
        self.env.pop('LD_PRELOAD', None)
        self.env.pop('WAYLAND_DISPLAY', None)
        self.log = (self.work / 'test.log').open('w+')
        sway = self.spawn(['sway', '--unsupported-gpu', '-c', '/dev/null'])
        sockets = []
        found = lambda: sockets.extend(p for p in self.work.glob('wayland-*') if p.is_socket()) or sockets
        if not wait_for(lambda: bool(found()) or sway.poll() is not None, 5) or not sockets:
            raise RuntimeError('headless Sway failed')
        self.env['WAYLAND_DISPLAY'] = sockets[0].name
        return self

    def __exit__(self, kind, value, trace):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self.log:
            if kind is not None:
                self.log.flush()
                self.log.seek(0)
                sys.stderr.write(self.log.read())
            self.log.close()
        if self.directory:
            self.directory.cleanup()
        return False

    def spawn(self, args, **kwargs):
        kwargs.setdefault('env', self.env)
        kwargs.setdefault('stdout', self.log)
        kwargs.setdefault('stderr', self.log)
        process = subprocess.Popen(args, **kwargs)
        self.processes.append(process)
        return process

    def device(self, name, *args):
        path = os.environ.get(f'V{name.upper()}', str(ROOT / f'build/tests/virtual-{name}'))
        return Device([path, *args], self.env, self.log, self.processes)

    def terminal(self, config, *args, **kwargs):
        path = self.work / 'term.conf'
        path.write_text(''.join(f'{line}\n' for line in config))
        binary = os.environ.get('BIN', str(ROOT / 'build/bin/wsap_term'))
        return self.spawn([binary, '--config', str(path), *args], **kwargs)
