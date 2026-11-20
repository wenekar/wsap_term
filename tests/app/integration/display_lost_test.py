import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]

def wait_for(predicate, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()

def answers(path):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(3)
            s.connect(str(path))
            s.sendall(b'windows\n')
            return json.loads(s.makefile('rb').readline()).get('ok', False)
    except (OSError, ValueError):
        return False

def main():
    missing = [tool for tool in ('weston', 'Xwayland') if not shutil.which(tool)]
    if missing:
        print('SKIP: needs ' + ', '.join(missing))
        raise SystemExit(77)
    processes = []
    with tempfile.TemporaryDirectory(prefix='wsap-x11-') as directory:
        work = Path(directory)
        log = (work / 'test.log').open('w+')
        env = dict(os.environ, XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY='wsap-test',
                   WSAP_CONFIG='NONE')
        env.pop('LD_PRELOAD', None)
        env.pop('DISPLAY', None)
        try:
            weston = subprocess.Popen(['weston', '--backend=headless', '--socket=wsap-test',
                                       '--idle-time=0', '--no-config'],
                                      env=env, stdout=log, stderr=log)
            processes.append(weston)
            if not wait_for(lambda: (work / 'wsap-test').is_socket(), 10):
                raise RuntimeError('headless Weston failed')
            display = work / 'display'
            with display.open('w') as fd:
                xwayland = subprocess.Popen(['Xwayland', '-displayfd', str(fd.fileno()),
                                             '-shm', '-noreset', '-nolisten', 'tcp'],
                                            env=env, pass_fds=(fd.fileno(),),
                                            stdout=log, stderr=log)
            processes.append(xwayland)
            if not wait_for(lambda: display.read_text().strip(), 10):
                raise RuntimeError('Xwayland failed')

            x11 = dict(env, DISPLAY=':' + display.read_text().strip(),
                       XDG_SESSION_TYPE='x11')
            x11.pop('WAYLAND_DISPLAY')
            config = work / 'term.conf'
            config.write_text('command = /bin/cat\nwidth = 800\nheight = 600\n')
            debug = work / 'debug.sock'
            binary = os.environ.get('BIN', str(ROOT / 'build/bin/wsap_term'))
            terminal = subprocess.Popen([binary, '--config', str(config),
                                         '--debug-socket', str(debug)],
                                        env=x11, stdout=log, stderr=log)
            processes.append(terminal)
            if not wait_for(lambda: answers(debug), 10):
                raise RuntimeError('the terminal never answered on its debug socket')

            xwayland.kill()
            xwayland.wait()
            if not wait_for(lambda: terminal.poll() is not None, 5):
                raise AssertionError('FAIL: the terminal still runs 5 s after its X '
                                     'server died')
            print(f'PASS: the terminal exited with status {terminal.returncode} after '
                  'its X server died')
        except BaseException:
            log.flush()
            log.seek(0)
            sys.stderr.write(log.read())
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log.close()

if __name__ == '__main__':
    main()
