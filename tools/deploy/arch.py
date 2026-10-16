
import subprocess
import tempfile
import time
from pathlib import Path

from common import PackageMetadata, stage_linux_payload

def _calculate_size(path: Path) -> int:
    total = 0
    for p in path.rglob("*"):
        if p.is_file() and not p.is_symlink():
            total += p.stat().st_size
    return total

def build_package(
    repo_root: Path, dist_dir: Path, meta: PackageMetadata
) -> tuple[Path, str]:
    dist_dir.mkdir(parents=True, exist_ok=True)
    pkg_name = f"{meta.name}-{meta.version}-{meta.release}-{meta.arch}.pkg.tar.zst"
    output_path = dist_dir / pkg_name

    with tempfile.TemporaryDirectory(prefix="wsap-arch-") as tmp:
        pkg_root = Path(tmp)
        usr_dir = pkg_root / "usr"
        stage_linux_payload(repo_root, usr_dir)

        size = _calculate_size(pkg_root)
        pkginfo_content = f"""pkgname = {meta.name}
pkgbase = {meta.name}
pkgver = {meta.version}-{meta.release}
pkgdesc = {meta.summary}
url = {meta.url}
builddate = {int(time.time())}
packager = {meta.maintainer}
size = {size}
arch = {meta.arch}
license = {meta.license}
depend = freetype2
depend = fontconfig
depend = harfbuzz
depend = wayland
depend = libxkbcommon
"""
        (pkg_root / ".PKGINFO").write_text(pkginfo_content, encoding="utf-8")

        tar_cmd = [
            "tar",
            "-C",
            str(pkg_root),
            "-c",
            "--zstd",
            "-f",
            str(output_path),
            ".PKGINFO",
            "usr",
        ]
        subprocess.check_call(tar_cmd)

    return output_path, f"sudo pacman -U {output_path}"
