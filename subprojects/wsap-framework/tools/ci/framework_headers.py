#!/usr/bin/env python3
"""Print the framework SDK header paths relative to the source root.

The framework SDK ships the public headers of the toolkit, window binding,
and desktop runner. Internal headers, font backends, and source-tree test
hooks are not installed.
"""

from __future__ import annotations

import sys
from pathlib import Path

SDK_DIRS = (
    "platform",
    "util",
    "loop",
    "input",
    "paint",
    "font",
    "ui",
    "theme",
    "render",
    "desktop",
)
PRIVATE_DIRS = ("ui/yasl/accessibility", "font/backends", "render/cpu", "render/gpu")
SDK_EXCLUDED_HEADERS = {
    "font/backend.h",
    "font/font5x7.h",
    "font/glyph/layout.h",
    "render/plan.h",
    "render/surface.h",
    "ui/yasl/native_input.h",
    "ui/yasl/testing.h",
}


def is_public(relative: str) -> bool:
    if relative in SDK_EXCLUDED_HEADERS:
        return False
    if relative.rsplit("/", 1)[-1].endswith("internal.h"):
        return False
    if "/internal/" in "/" + relative:
        return False
    return not any(relative == item or relative.startswith(item + "/")
                   for item in PRIVATE_DIRS)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: framework_headers.py SRC_DIR", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    for directory in SDK_DIRS:
        for path in sorted((root / directory).rglob("*.h")) + sorted((root / directory).rglob("*.inc")):
            relative = path.relative_to(root).as_posix()
            if is_public(relative):
                print(relative)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
