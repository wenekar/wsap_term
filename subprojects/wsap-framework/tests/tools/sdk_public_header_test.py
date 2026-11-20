"""The SDK ships the desktop and renderer APIs without terminal or test hooks."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TERMINAL_API = ("struct terminal_frame", "render_draw(", "render_cursor_cell(",
                "render_gpu_draw(")
REQUIRED = {"desktop/window.h", "render/render.h", "render/grid.h", "render/isa.h"}
SOURCE_ONLY = {"render/cpu/kernels.h", "render/gpu/build.h", "ui/yasl/testing.h"}


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
    paths = set(public_headers())
    bad = [f"missing SDK header: {path}" for path in sorted(REQUIRED - paths)]
    bad += [f"installed source-only header: {path}" for path in sorted(SOURCE_ONLY & paths)]
    for relative in sorted(paths):
        if not relative.endswith(".h"):
            continue
        text = (ROOT / "src" / relative).read_text()
        for symbol in TERMINAL_API:
            if symbol in text:
                bad.append(f"{relative} names terminal API: {symbol.rstrip('(')}")
    if bad:
        print(*bad, sep="\n", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
