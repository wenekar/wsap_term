"""Exercise output routing through the actual preload library and exec boundary."""

import errno
import os
from pathlib import Path
import pty
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

def drain(fd):
    os.set_blocking(fd, False)
    data = bytearray()
    while True:
        try:
            part = os.read(fd, 4096)
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EIO):
                break
            raise
        if not part:
            break
        data.extend(part)
    return bytes(data)

library, helper = map(os.path.abspath, sys.argv[1:])
fish = shutil.which("fish")
with tempfile.TemporaryDirectory(prefix="fastpty-test-") as directory:
    sudo = Path(directory) / "sudo"
    sudo.symlink_to(helper)
    doas = Path(directory) / "doas"
    doas.symlink_to(helper)
    agy = Path(directory) / "agy"
    agy.symlink_to(helper)
    bwrap = Path(directory) / "bwrap"
    bwrap.symlink_to(helper)
    codex = Path(directory) / "codex"
    codex.symlink_to(helper)
    ordinary = Path(directory) / "ordinary"
    ordinary.symlink_to(helper)
    real_sudo_dir = Path(directory) / "realbin"
    real_sudo_dir.mkdir()
    real_sudo = real_sudo_dir / "sudo"
    shutil.copy(helper, real_sudo)
    scenarios = ["sudo", "doas", "agy", "bwrap", "codex", "spawn", "spawnp", "stdout-file", "stderr-file",
                 "pipeline", "ordinary", "failed", "vp", "vp-isatty",
                 "termios", "other-termios", "dup", "reuse", "ttyname", "tc",
                 "v", "l", "le", "lp", "fexec",
                 "v-isatty", "l-isatty", "le-isatty", "lp-isatty", "fexec-isatty",
                 "spawn-isatty", "spawnp-isatty", "vp-env", "v-isatty-stderr"]
    if fish:
        scenarios.append("fish")
    for scenario in scenarios:
        master, slave = pty.openpty()
        fast_read, fast_write = os.pipe()
        user_read, user_write = os.pipe()
        try:
            with tempfile.TemporaryFile() as redirected:
                env = dict(os.environ, LD_PRELOAD=library,
                           WSAP_PIPE_ID="%d:%d" % (os.fstat(fast_write).st_dev,
                                                         os.fstat(fast_write).st_ino),
                           WSAP_PTY_PATH=os.ttyname(slave))
                stdout = redirected if scenario in ("stdout-file", "v-isatty-stderr") else fast_write
                stderr = redirected if scenario == "stderr-file" else fast_write
                if scenario == "pipeline":
                    stdout = user_write
                if (scenario == "ordinary" or "-isatty" in scenario
                        or scenario == "vp-env"):
                    target = ordinary
                elif scenario == "fexec":
                    target = real_sudo
                elif scenario == "doas":
                    target = doas
                elif scenario == "agy":
                    target = agy
                elif scenario == "bwrap":
                    target = bwrap
                elif scenario == "codex":
                    target = codex
                else:
                    target = sudo
                mode = scenario
                if scenario in ("sudo", "doas", "agy", "bwrap", "codex", "stdout-file", "stderr-file",
                                "pipeline", "ordinary"):
                    mode = "exec"
                if scenario in ("spawnp", "spawnp-isatty", "lp", "lp-isatty"):
                    env["PATH"] = f"{directory}:{env.get('PATH', '')}"
                    target = Path(target).name
                if scenario == "failed":
                    target = Path(directory) / "missing" / "sudo"
                    mode = "fail"
                if scenario.startswith("termios"):
                    command = [helper, "probe-termios"]
                elif scenario in ("ttyname", "tc", "other-termios", "dup", "reuse"):
                    command = [helper, f"probe-{scenario}"]
                else:
                    command = [helper, mode, str(target)]
                if scenario == "fish":
                    env["fish_use_posix_spawn"] = "0"
                    command = [fish, "--no-config", "-c",
                               f"{shlex.quote(str(sudo))} probe; {shlex.quote(helper)} probe"]
                subprocess.run(command, env=env, stdin=slave,
                               stdout=stdout, stderr=stderr,
                               check=True, timeout=5)
                terminal = drain(master).replace(b"\r\n", b"\n")
                fast = drain(fast_read)
                pipeline = drain(user_read)
                redirected.seek(0)
                file_output = redirected.read()
                tty_out = b"OUT:tty\nERR:tty\n"
                isatty_out = b"ISATTY:1\n"
                expected = {
                    "sudo": (tty_out, b"", b"", b""),
                    "doas": (tty_out, b"", b"", b""),
                    "agy": (tty_out, b"", b"", b""),
                    "bwrap": (tty_out, b"", b"", b""),
                    "codex": (tty_out, b"", b"", b""),
                    "spawn": (tty_out, b"", b"", b""),
                    "spawnp": (tty_out, b"", b"", b""),
                    "stdout-file": (b"ERR:tty\n", b"", b"", b"OUT:file\n"),
                    "stderr-file": (b"OUT:tty\n", b"", b"", b"ERR:file\n"),
                    "pipeline": (b"ERR:tty\n", b"", b"OUT:pipe\n", b""),
                    "ordinary": (b"", b"OUT:pipe\nERR:pipe\n", b"", b""),
                    "failed": (b"", b"OUT:restored\nERR:restored\n", b"", b""),
                    "vp": (tty_out, b"", b"", b""),
                    "vp-isatty": (b"", isatty_out, b"", b""),
                    "v": (tty_out, b"", b"", b""),
                    "l": (tty_out, b"", b"", b""),
                    "le": (tty_out, b"", b"", b""),
                    "lp": (tty_out, b"", b"", b""),
                    "fexec": (tty_out, b"", b"", b""),
                    "v-isatty": (b"", isatty_out, b"", b""),
                    "l-isatty": (b"", isatty_out, b"", b""),
                    "le-isatty": (b"", isatty_out, b"", b""),
                    "lp-isatty": (b"", isatty_out, b"", b""),
                    "fexec-isatty": (b"", isatty_out, b"", b""),
                    "spawn-isatty": (b"", isatty_out, b"", b""),
                    "spawnp-isatty": (b"", isatty_out, b"", b""),
                    "vp-env": (b"", f"PRELOAD:{library}\n".encode(), b"", b""),
                    "v-isatty-stderr": (b"", isatty_out, b"", b""),
                    "ttyname": (b"", f"TTYNAME:{os.ttyname(slave)}\n".encode(), b"", b""),
                    "tc": (b"", b"PGRP:ok\nDRAIN:0\nFLOW:0\nFLUSH:0\n", b"", b""),
                    "other-termios": (b"", b"OTHER:ok\n", b"", b""),
                    "dup": (b"", b"DUP:ok\n", b"", b""),
                    "reuse": (b"", b"REUSE:ok\n", b"", b""),
                    "termios": None,
                    "fish": (tty_out, b"OUT:pipe\nERR:pipe\n", b"", b""),
                }[scenario]
                if scenario.startswith("termios"):
                    # The probe's own output and nothing else: a fullmatch here
                    # is what proves the shim injects no side-channel bytes.
                    match = re.fullmatch(rb"OFLAG:(\d+):(\d+):(\d+)\n", fast)
                    assert match, fast
                    expected = (b"", match.group(0), b"", b"")
                actual = terminal, fast, pipeline, file_output
                assert actual == expected, (scenario, actual, expected)
        finally:
            for fd in (master, slave, fast_read, fast_write, user_read, user_write):
                os.close(fd)
print("PASS: privilege-command PTY fallback, redirection, pipeline, exec routing/propagation, failed-exec, termios pass-through, ttyname, and tc* tests")
