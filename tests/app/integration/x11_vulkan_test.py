"""Test the Vulkan path of wsap_term on X11 / Xwayland."""

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


def main():
    for tool in ('sway', 'swaymsg', 'grim'):
        if not shutil.which(tool):
            print(f'SKIP: needs {tool}')
            raise SystemExit(77)

    with tempfile.TemporaryDirectory(prefix='wsap-x11-vulkan-') as directory:
        work = Path(directory)
        sway_conf = work / 'sway.conf'
        display_file = work / 'x11_display'
        sway_conf.write_text(
            'xwayland enable\n'
            'output HEADLESS-1 mode 1920x1080\n'
            'default_border none\n'
            'floating_maximum_size -1 x -1\n'
            'floating_minimum_size 50 x 50\n'
            'for_window [app_id=".*"] floating enable\n'
            'for_window [instance=".*"] floating enable\n'
            f'exec echo "$DISPLAY" > {display_file}\n'
        )

        sway_env = dict(
            os.environ,
            XDG_RUNTIME_DIR=directory,
            WLR_BACKENDS='headless',
            WLR_LIBINPUT_NO_DEVICES='1',
            WLR_RENDERER='gles2',
        )
        sway_env.pop('WAYLAND_DISPLAY', None)
        sway_env.pop('DISPLAY', None)

        sway_log = (work / 'sway.log').open('w+')
        sway_proc = subprocess.Popen(
            ['sway', '--unsupported-gpu', '-c', str(sway_conf)],
            env=sway_env,
            stdout=sway_log,
            stderr=sway_log,
        )

        term_proc = None
        try:
            if not wait_for(lambda: display_file.exists() and display_file.read_text().strip(), 10):
                raise RuntimeError('Sway Xwayland did not start')
            x11_display = display_file.read_text().strip()

            ipc_names = [n for n in os.listdir(directory) if n.startswith('sway-ipc.')]
            if not ipc_names:
                raise RuntimeError('Sway IPC socket not found')
            ipc_path = os.path.join(directory, ipc_names[0])

            wayland_displays = [n for n in os.listdir(directory) if n.startswith('wayland-') and not n.endswith('.lock')]
            if not wayland_displays:
                raise RuntimeError('Sway Wayland display not found')
            wayland_display = wayland_displays[0]

            app_env = dict(
                os.environ,
                XDG_RUNTIME_DIR=directory,
                DISPLAY=x11_display,
                WSAP_CONFIG='NONE',
            )
            app_env.pop('WAYLAND_DISPLAY', None)

            # Enable validation layer when available
            app_env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'

            dbg_sock = work / 'dbg.sock'
            term_conf = work / 'term.conf'
            term_conf.write_text(
                'width = 800\n'
                'height = 600\n'
                'background = #1a1b26\n'
                'command = /bin/cat\n'
            )

            binary = os.environ.get('BIN', str(ROOT / 'build/bin/wsap_term'))
            term_log = (work / 'term.log').open('w+')
            term_proc = subprocess.Popen(
                [binary, '--config', str(term_conf), '--debug-socket', str(dbg_sock)],
                env=app_env,
                stdout=term_log,
                stderr=term_log,
            )

            if not wait_for(lambda: ask(dbg_sock, 'windows') is not None, 10):
                raise RuntimeError('Terminal did not answer on debug socket')

            # Verify window is running under Xwayland
            tree_out = subprocess.run(['swaymsg', '-s', ipc_path, '-t', 'get_tree'],
                                      stdout=subprocess.PIPE, text=True, check=True)
            tree = json.loads(tree_out.stdout)

            def find_win(node):
                if node.get('shell') == 'xwayland' and node.get('rect'):
                    return node
                for c in node.get('nodes', []) + node.get('floating_nodes', []):
                    w = find_win(c)
                    if w:
                        return w
                return None

            win_node = find_win(tree)
            if not win_node:
                raise RuntimeError('Window not found in Sway tree as xwayland shell')

            grim_env = dict(os.environ, XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY=wayland_display)

            # Test multiple resizes
            sizes = [(800, 600), (1024, 768), (640, 480), (1280, 720), (500, 350)]
            for idx, (w, h) in enumerate(sizes):
                cmd = f'resize set width {w} px height {h} px'
                subprocess.run(['swaymsg', '-s', ipc_path, cmd], check=True, stdout=subprocess.DEVNULL)
                time.sleep(0.2)

                reply = ask(dbg_sock, 'windows')
                if not reply or not reply.get('ok'):
                    raise RuntimeError(f'Debug socket query failed after resize to {w}x{h}')
                reported = reply['result'][0]['size']
                if reported != [w, h]:
                    raise AssertionError(f'Expected size {[w, h]}, got {reported}')

                lint = ask(dbg_sock, 'lint')
                if not lint or lint['result'].get('count', 0) != 0:
                    raise AssertionError(f'Lint reported issues: {lint}')

                # Capture frame via grim
                ss_file = work / f'ss_{idx}_{w}x{h}.png'
                subprocess.run(['grim', str(ss_file)], env=grim_env, check=True)
                if not ss_file.exists() or ss_file.stat().st_size == 0:
                    raise RuntimeError(f'Screenshot failed for size {w}x{h}')

            term_proc.terminate()
            term_proc.wait(timeout=5)

            term_log.flush()
            term_log.seek(0)
            err_text = term_log.read()
            if 'Validation Error' in err_text:
                raise AssertionError(f'Vulkan validation error detected:\n{err_text}')

            print('PASS: Vulkan path on X11 resized cleanly with no validation errors')
        finally:
            if term_proc and term_proc.poll() is None:
                term_proc.kill()
                term_proc.wait()
            if sway_proc.poll() is None:
                sway_proc.kill()
                sway_proc.wait()
            sway_log.close()
            term_log.close()


if __name__ == '__main__':
    main()
