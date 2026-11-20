"""The Linux package payload holds the bundled fonts and all license texts."""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "deploy"))

from common import stage_linux_payload


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / "repo"
        build = Path(tmp) / "build"
        binary = build / "bin" / "wsap_term"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/bin/sh\n")
        binary.chmod(0o755)
        fonts = repo / "assets" / "fonts"
        fonts.mkdir(parents=True)
        (repo / "LICENSE").write_text("license\n")
        (fonts / "FiraCodeNerdFontMono-Regular.ttf").write_bytes(b"font")
        (fonts / "LICENSE").write_text("OFL\n")
        third_party = repo / "subprojects" / "wsap-framework" / "LICENSES"
        third_party.mkdir(parents=True)
        (third_party / "NOTICE.md").write_text("notices\n")
        (third_party / "accesskit-c-MIT.txt").write_text("MIT\n")

        stage = Path(tmp) / "stage"
        old = os.environ.get("MESON_BUILD_ROOT")
        os.environ["MESON_BUILD_ROOT"] = str(build)
        try:
            stage_linux_payload(repo, stage)
        finally:
            if old is None:
                del os.environ["MESON_BUILD_ROOT"]
            else:
                os.environ["MESON_BUILD_ROOT"] = old

        staged = stage / "share" / "wsap_term" / "fonts"
        assert (staged / "FiraCodeNerdFontMono-Regular.ttf").is_file(), \
            "the payload has no bundled font"
        assert (staged / "LICENSE").is_file(), "the payload has no font licence"
        licenses = stage / "share" / "licenses" / "wsap_term"
        for name in ("LICENSE", "NOTICE.md", "accesskit-c-MIT.txt"):
            assert (licenses / name).is_file(), f"the payload has no {name}"


if __name__ == "__main__":
    main()
