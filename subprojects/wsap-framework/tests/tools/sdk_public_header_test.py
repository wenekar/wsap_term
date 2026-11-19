"""The installed paint headers name no terminal render call.

paint/framebuffer.h ships to SDK consumers. The terminal renderer is not part
of the SDK, so the header must not name its draw calls.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TERMINAL_CALLS = ("render_draw(", "render_cursor_cell(", "render_gpu_draw(")


def public_headers() -> list[str]:
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "ci" / "framework_headers.py"),
         str(ROOT / "src")],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.split()


def main() -> None:
    bad = []
    for relative in public_headers():
        if not relative.startswith("paint/"):
            continue
        text = (ROOT / "src" / relative).read_text()
        for call in TERMINAL_CALLS:
            if call in text:
                bad.append(f"{relative}: {call[:-1]}")
    if bad:
        print("the installed paint headers name a terminal render call:",
              *bad, sep="\n  ", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
