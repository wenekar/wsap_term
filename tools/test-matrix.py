#!/usr/bin/env python3
import argparse
from pathlib import Path
import platform
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-root', type=Path, default=Path('build/matrix'))
parser.add_argument('--isas', nargs='*', default=None,
                    help='ISA targets; each must be supported by the compiler and CPU')
parser.add_argument('--window-backend', choices=['auto', 'wayland', 'metal'], default='auto')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
isas = args.isas if args.isas is not None else (
    ['x86-64-v2', 'x86-64-v3', 'znver5'] if platform.machine() in ('x86_64', 'AMD64') else [])
status = 0
for index, isa in enumerate(['native', *isas]):
    build = args.build_root.resolve() / ('native' if index == 0 else f'isa-{index}')
    setup = ['meson', 'setup', str(build), str(root), '--buildtype=debugoptimized',
             '-Doptimization=1', '-Db_lto=false', '-Db_lundef=false', '-Db_sanitize=address,undefined',
             f'-Disa={isa}', f'-Dwindow_backend={args.window_backend}']
    if (build / 'meson-private/coredata.dat').exists():
        setup.append('--reconfigure')
    print(f'=== Testing {isa} ===', flush=True)
    result = subprocess.run(setup)
    if result.returncode == 0:
        command = ['meson', 'test', '-C', str(build), '--print-errorlogs']
        if index:
            command += ['--suite', 'isa']
        result = subprocess.run(command)
    status |= int(result.returncode != 0)
    print(f'=== {isa}: {"PASS" if result.returncode == 0 else "FAIL"} ===', flush=True)
raise SystemExit(status)
