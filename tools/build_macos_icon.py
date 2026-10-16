#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

def main():
    icons = Path(__file__).resolve().parent.parent / "assets" / "icons"
    with tempfile.TemporaryDirectory(prefix="wsap-icon-") as tmp:
        iconset = Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = "@2x" if scale == 2 else ""
                subprocess.run([
                    "resvg", "--width", str(size * scale),
                    str(icons / "wsap-term.svg"),
                    str(iconset / f"icon_{size}x{size}{suffix}.png"),
                ], check=True)
        subprocess.run([
            "iconutil", "--convert", "icns", "--output",
            str(icons / "wsap-term.icns"), str(iconset),
        ], check=True)

if __name__ == "__main__":
    main()
