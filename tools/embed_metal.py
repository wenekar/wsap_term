"""Make a C string from the Metal source."""

import json
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text()
lines = ['static NSString *const kMetalShaderSource =']
lines += [('    @' if i == 0 else '    ') + json.dumps(line)
          for i, line in enumerate(source.splitlines(keepends=True))]
Path(sys.argv[2]).write_text('\n'.join(lines) + ';\n')
