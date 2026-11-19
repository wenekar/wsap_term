"""Require the shared library to export exactly the public header functions."""

import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-export', action='append', default=[])
    parser.add_argument('--platform', choices=['linux', 'darwin'], default='linux')
    parser.add_argument('nm')
    parser.add_argument('library')
    parser.add_argument('headers', nargs='+')
    args = parser.parse_args()
    declarations = re.compile(
        r"^\s*YASL_API\b[^;{}]*?\b(yasl_\w+)\s*\([^;{}]*\);", re.MULTILINE
    )
    expected = set()
    for header in args.headers:
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", Path(header).read_text(), flags=re.DOTALL)
        expected.update(declarations.findall(source))
    if not expected:
        raise SystemExit("No public function declarations found")

    nm_options = ["-g", "-U", "-P"] if args.platform == 'darwin' else [
        "-D", "--defined-only", "--format=posix"
    ]
    output = subprocess.check_output([args.nm, *nm_options, args.library], text=True)
    exported = {line.split()[0] for line in output.splitlines() if line.strip()}
    if args.platform == 'darwin':
        # Mach-O decorates C symbols with one leading underscore.
        exported = {name[1:] if name.startswith('_') else name for name in exported}
    missing = expected - exported
    leaked = exported - expected - set(args.allow_export)
    if missing or leaked:
        if missing:
            print("Missing public exports:", ", ".join(sorted(missing)))
        if leaked:
            print("Unexpected exports:", ", ".join(sorted(leaked)))
        raise SystemExit(1)
    print(f"Verified {len(expected)} public exports")


if __name__ == "__main__":
    main()
