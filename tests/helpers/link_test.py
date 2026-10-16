
import os
import sys
from pathlib import Path

target, output = map(Path, sys.argv[1:])
output.unlink(missing_ok=True)
output.symlink_to(os.path.relpath(target, output.parent))
