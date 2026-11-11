"""Select the graphics drivers of the display tests."""
import json
import os
from pathlib import Path
import struct


def lavapipe_icd():
    """Return the lavapipe driver file for the word size of this machine, or None.

    A system can have a 32-bit and a 64-bit file. The loader cannot load a
    driver of the other word size, so the first file is not always correct."""
    bits = str(struct.calcsize('P') * 8)
    for path in sorted(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json')):
        try:
            arch = json.loads(path.read_text())['ICD'].get('library_arch', bits)
        except (OSError, ValueError, KeyError, AttributeError):
            continue
        if arch == bits:
            return path
    return None


def graphics_env(env):
    """Return the environment changes that select the drivers.

    With a render node, the compositor and the clients use the drivers that
    the system selects. Without one, or when LIBGL_ALWAYS_SOFTWARE=1 is set,
    they use llvmpipe and lavapipe. The compositor and the clients must agree:
    a compositor on llvmpipe cannot import the buffers of a hardware Vulkan
    driver, and Weston 15 stops with a segmentation fault when it tries."""
    nodes = Path('/dev/dri').glob('renderD*')
    if env.get('LIBGL_ALWAYS_SOFTWARE') != '1' and any(
            os.access(node, os.R_OK | os.W_OK) for node in nodes):
        return {}
    changes = {'LIBGL_ALWAYS_SOFTWARE': '1'}
    lavapipe = lavapipe_icd()
    if lavapipe:
        changes['VK_DRIVER_FILES'] = str(lavapipe)
    return changes
