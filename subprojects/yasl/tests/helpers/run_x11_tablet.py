#!/usr/bin/env python3
"""Run pen pressure through Xorg's virtual input driver and the example app."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    xorg, test = sys.argv[1:]
    module_path = os.environ.get("YASL_XORG_MODULE_PATH")
    if not module_path:
        module_path = subprocess.check_output(
            [xorg, "-showDefaultModulePath"], text=True, timeout=5).strip()
    if any(c in module_path for c in ('"', "\n", "\r")):
        raise ValueError("Invalid Xorg module path")
    paths = [Path(path) for path in module_path.split(",")]
    for module in ("drivers/dummy_drv.so", "input/inputtest_drv.so"):
        if not any((path / module).is_file() for path in paths):
            raise RuntimeError(f"Missing Xorg module: {module}")
    with tempfile.TemporaryDirectory(prefix="yasl-tablet-") as directory:
        root = Path(directory)
        pen, eraser = root / "pen.sock", root / "eraser.sock"
        config = root / "xorg.conf"
        config.write_text(f'''
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoAddGPU" "false"
    Option "DontVTSwitch" "true"
EndSection
Section "Files"
    ModulePath "{module_path}"
EndSection
Section "Module"
    Disable "glx"
EndSection
Section "Device"
    Identifier "virtual-video"
    Driver "dummy"
    VideoRam 16384
EndSection
Section "Monitor"
    Identifier "virtual-monitor"
    HorizSync 5.0 - 1000.0
    VertRefresh 5.0 - 200.0
    Modeline "1200x900" 96.96 1200 1280 1408 1616 900 901 904 932
EndSection
Section "Screen"
    Identifier "virtual-screen"
    Device "virtual-video"
    Monitor "virtual-monitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1200x900"
    EndSubSection
EndSection
Section "InputDevice"
    Identifier "YASL Test Pen"
    Driver "inputtest"
    Option "DeviceType" "PointerAbsoluteProximity"
    Option "PointerHasPressure" "true"
    Option "SocketPath" "{pen}"
EndSection
Section "InputDevice"
    Identifier "YASL Test Reverse Tool"
    Driver "inputtest"
    Option "DeviceType" "PointerAbsoluteProximity"
    Option "PointerHasPressure" "true"
    Option "SocketPath" "{eraser}"
EndSection
Section "InputDevice"
    Identifier "YASL Test Keyboard"
    Driver "inputtest"
    Option "DeviceType" "Keyboard"
    Option "SocketPath" "{root / 'keyboard.sock'}"
EndSection
Section "ServerLayout"
    Identifier "virtual-layout"
    Screen "virtual-screen"
    InputDevice "YASL Test Pen" "CorePointer"
    InputDevice "YASL Test Reverse Tool" "SendCoreEvents"
    InputDevice "YASL Test Keyboard" "CoreKeyboard"
EndSection
''')
        display_file = root / "display"
        with display_file.open("w") as display_fd, (root / "server.log").open("w+") as log:
            server = subprocess.Popen([
                xorg, "-config", str(config), "-logfile", str(root / "xorg.log"),
                "-displayfd", str(display_fd.fileno()), "-noreset", "-nolisten", "tcp",
                "-novtswitch", "-sharevts", "-seat", f"yasl-tablet-test-{os.getpid()}",
            ], pass_fds=(display_fd.fileno(),), stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                while not (display_file.read_text().strip() and pen.is_socket() and eraser.is_socket()):
                    if server.poll() is not None:
                        raise RuntimeError("The virtual X server stopped")
                    if time.monotonic() >= deadline:
                        raise TimeoutError("The virtual X server did not start")
                    time.sleep(0.02)
                env = os.environ.copy()
                env["DISPLAY"] = ":" + display_file.read_text().strip()
                env.pop("XAUTHORITY", None)
                subprocess.run([test, str(pen), str(eraser)], env=env, check=True, timeout=10)
            except BaseException:
                log.flush()
                log.seek(0, 2)
                log.seek(max(0, log.tell() - 8000))
                print(log.read(), file=sys.stderr)
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == "__main__":
    main()
