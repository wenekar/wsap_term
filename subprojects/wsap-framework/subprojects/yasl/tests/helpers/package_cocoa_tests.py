#!/usr/bin/env python3
"""Package compiled Cocoa tests for execution on a matching macOS desktop."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    info = build / 'meson-info'
    host = json.loads((info / 'intro-machines.json').read_text())['host']
    if host['system'] != 'darwin':
        parser.error('Use a configured and compiled Cocoa build.')
    tests = json.loads((info / 'intro-tests.json').read_text())
    required = {'surface-cocoa', 'native-cocoa', 'ownership-cocoa', 'keyboard-cocoa',
                'tablet-cocoa', 'cursor-cocoa'}
    missing = required - {test['name'] for test in tests}
    if missing:
        parser.error('Enable integration_tests; missing: ' + ', '.join(sorted(missing)))
    options = json.loads((info / 'intro-buildoptions.json').read_text())
    sanitizer = next(option['value'] for option in options if option['name'] == 'b_sanitize')
    if sanitizer not in ('none', [], ['none']):
        parser.error('Use an unsanitized build; sanitizer runtimes are not bundled.')

    with tempfile.TemporaryDirectory(prefix='yasl-cocoa-bundle-') as directory:
        root = Path(directory) / 'yasl-cocoa-tests'
        root.mkdir()
        files = {}

        def copy(source, relative):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            files[str(relative)] = hashlib.sha256(destination.read_bytes()).hexdigest()

        copy(build / 'libyasl.dylib', Path('libyasl.dylib'))
        source_root = Path(__file__).resolve().parents[2]
        copy(source_root / 'LICENSE', Path('LICENSE'))
        copy(Path(__file__).with_name('run_cocoa_bundle.py'), Path('run.py'))
        commands = []
        for test in tests:
            if test['name'] == 'public-exports':
                continue  # Check the dylib with nm on the build machine first.
            executable = Path(test['cmd'][0]).resolve()
            try:
                relative = executable.relative_to(build)
            except ValueError:
                parser.error('Unexpected external test command: ' + str(executable))
            copy(executable, relative)
            commands.append({'name': test['name'], 'cmd': [str(relative), *test['cmd'][1:]],
                             'timeout': test['timeout']})
        manifest = {'architecture': host['cpu_family'], 'minimum_macos': 14,
                    'tests': commands, 'sha256': files}
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with tarfile.open(args.output, 'w:gz') as archive:
            archive.add(root, arcname=root.name)
    print(f'Packaged {len(commands)} runtime tests for {host["cpu_family"]}: {args.output}')


if __name__ == '__main__':
    main()
