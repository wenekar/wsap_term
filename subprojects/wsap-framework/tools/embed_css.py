"""Embeds a CSS file in a C header.

Usage: embed_css.py SYMBOL INPUT OUTPUT

The output defines `SYMBOL`, a static C string that holds the text of the
input file and ends with a NUL byte. The build writes the header to its own
directory, so the source tree stays free of generated files.
"""

import sys
from pathlib import Path


def c_literal(line):
    out = ['"']
    for ch in line:
        if ch in ('"', '\\'):
            out.append('\\' + ch)
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) >= 0x7F:
            out.append('\\%03o' % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    symbol, source, output = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    lines = source.read_text().splitlines(keepends=True)
    body = ['/* Generated from %s. Do not edit. */' % source.name,
            'static const char %s[] =' % symbol]
    body += ['    ' + c_literal(line) for line in lines]
    if not lines:
        body.append('    "";')
    elif lines[-1].endswith('\n'):
        body[-1] += ';'
    else:
        body[-1] += ' "\\n";'
    output.write_text('\n'.join(body) + '\n')


main()
