"""Embeds a Metal shader source in an Objective-C header.

Usage: embed_metal.py SOURCE OUTPUT [INCLUDE_DIR ...]

The output defines `kMetalShaderSource`, an NSString. A line that reads
`#include "path"` is replaced by the file. The script looks for the file in
the directory of the including file and then in each INCLUDE_DIR. A file
that was included once is not included again. Angle bracket includes stay
as they are, because the Metal compiler resolves them.
"""

import json
import re
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"\s*$')


def expand(path, include_dirs, seen):
    lines = []
    for line in path.read_text().splitlines(keepends=True):
        match = INCLUDE.match(line)
        if not match:
            lines.append(line)
            continue
        name = match.group(1)
        found = None
        for directory in [path.parent, *include_dirs]:
            candidate = (directory / name).resolve()
            if candidate.is_file():
                found = candidate
                break
        if found is None:
            sys.exit(f'embed_metal.py: {path}: cannot find "{name}"')
        if found in seen:
            continue
        seen.add(found)
        text = expand(found, include_dirs, seen)
        lines.append(text if text.endswith('\n') else text + '\n')
    return ''.join(lines)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    source = Path(sys.argv[1]).resolve()
    include_dirs = [Path(arg) for arg in sys.argv[3:]]
    text = expand(source, include_dirs, {source})
    lines = ['static NSString *const kMetalShaderSource =']
    lines += [('    @' if i == 0 else '    ') + json.dumps(line)
              for i, line in enumerate(text.splitlines(keepends=True))]
    Path(sys.argv[2]).write_text('\n'.join(lines) + ';\n')


main()
