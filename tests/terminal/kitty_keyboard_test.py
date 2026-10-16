#!/usr/bin/env python3
import os
import select
import termios
import tty

def main():
    fd = os.open('/dev/tty', os.O_RDWR)
    saved = termios.tcgetattr(fd)
    pushed = False
    try:
        tty.setraw(fd)
        os.write(fd, b'Kitty keyboard test: try letters, modifiers, arrows, and held keys.\r\n'
                     b'Each received chunk is shown as Python bytes. Press Escape to finish.\r\n'
                     b'Query reply should be \\x1b[?31u; repeats use :2 and releases :3.\r\n')
        os.write(fd, b'\x1b[>31u')
        pushed = True
        os.write(fd, b'\x1b[?u')
        pending = b''
        while True:
            if not select.select([fd], [], [], 60)[0]:
                break
            data = os.read(fd, 4096)
            if not data:
                break
            os.write(fd, repr(data).encode('ascii') + b'\r\n')
            pending = (pending + data)[-8192:]
            if b'\x1b[27;' in pending or b'\x1b[27u' in pending or b'\x03' in data:
                break
    finally:
        if pushed:
            os.write(fd, b'\x1b[<u')
        termios.tcsetattr(fd, termios.TCSANOW, saved)
        os.close(fd)

if __name__ == '__main__':
    main()
