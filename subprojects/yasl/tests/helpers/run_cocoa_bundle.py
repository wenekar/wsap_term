#!/usr/bin/env python3
"""Run a packaged Cocoa suite, preserving output and a machine-readable report."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true', help='List tests without executing them.')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    manifest = json.loads((root / 'manifest.json').read_text())
    if args.list:
        print('Architecture:', manifest['architecture'])
        for test in manifest['tests']:
            print(test['name'])
        return
    if sys.platform != 'darwin':
        parser.error('Run this suite in a macOS desktop session.')
    if int(platform.mac_ver()[0].split('.')[0]) < manifest['minimum_macos']:
        parser.error('This suite requires macOS 14 or newer.')
    architecture = {'arm64': 'aarch64'}.get(platform.machine(), platform.machine())
    if architecture != manifest['architecture']:
        parser.error('Use the bundle matching this Python process architecture.')
    for relative, expected in manifest['sha256'].items():
        if hashlib.sha256((root / relative).read_bytes()).hexdigest() != expected:
            parser.error('Bundle file changed: ' + relative)

    logs = root / 'logs'
    logs.mkdir(exist_ok=True)
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(root)
    results = []
    for test in manifest['tests']:
        name = test['name']
        log = logs / (re.sub(r'[^A-Za-z0-9_-]', '_', name) + '.log')
        command = [str(root / test['cmd'][0]), *test['cmd'][1:]]
        with log.open('w') as output:
            try:
                process = subprocess.run(command, cwd=root, env=env, stdout=output,
                                         stderr=subprocess.STDOUT, timeout=test['timeout'] or None)
                result = {'name': name, 'returncode': process.returncode,
                          'status': 'pass' if process.returncode == 0 else 'fail'}
            except subprocess.TimeoutExpired:
                output.write('\nTest timed out.\n')
                result = {'name': name, 'status': 'timeout'}
            except OSError as error:
                output.write(str(error) + '\n')
                result = {'name': name, 'status': 'error'}
        results.append(result)
        print(f'{name}: {result["status"]}', flush=True)
    report = root / 'results.json'
    report.write_text(json.dumps(results, indent=2) + '\n')
    failed = sum(result['status'] != 'pass' for result in results)
    print(f'{len(results) - failed} passed, {failed} failed. Logs: {logs}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
