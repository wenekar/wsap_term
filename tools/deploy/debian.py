
import shutil
import subprocess
import tempfile
from pathlib import Path

from common import PackageMetadata, stage_linux_payload

def _calculate_installed_size_kb(path: Path) -> int:
    total = 0
    for p in path.rglob("*"):
        if p.is_file() and not p.is_symlink():
            total += p.stat().st_size
    return max(1, total // 1024)

def _build_deb_fallback(pkg_root: Path, output_path: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="deb-build-") as tmp:
        tmp_dir = Path(tmp)
        (tmp_dir / "debian-binary").write_text("2.0\n", encoding="utf-8")

        control_tar = tmp_dir / "control.tar.gz"
        subprocess.check_call(
            [
                "tar",
                "-czf",
                str(control_tar),
                "-C",
                str(pkg_root / "DEBIAN"),
                "control",
            ],
        )

        data_tar = tmp_dir / "data.tar.xz"
        subprocess.check_call(
            ["tar", "-cJf", str(data_tar), "-C", str(pkg_root), "usr"],
        )

        ar = shutil.which("ar")
        if not ar:
            raise RuntimeError(
                "Neither 'dpkg-deb' nor 'ar' found to assemble .deb package."
            )

        subprocess.check_call(
            [
                ar,
                "rc",
                str(output_path),
                str(tmp_dir / "debian-binary"),
                str(control_tar),
                str(data_tar),
            ],
        )

def build_package(
    repo_root: Path, dist_dir: Path, meta: PackageMetadata
) -> tuple[Path, str]:
    dist_dir.mkdir(parents=True, exist_ok=True)
    pkg_file = f"{meta.pkg_name_deb}_{meta.version}-{meta.release}_{meta.deb_arch}.deb"
    output_path = dist_dir / pkg_file

    with tempfile.TemporaryDirectory(prefix="wsap-deb-") as tmp:
        pkg_root = Path(tmp)
        stage_linux_payload(repo_root, pkg_root / "usr")
        copyright_dir = pkg_root / "usr" / "share" / "doc" / meta.pkg_name_deb
        copyright_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo_root / "LICENSE", copyright_dir / "copyright")

        debian_dir = pkg_root / "DEBIAN"
        debian_dir.mkdir(parents=True, exist_ok=True)
        size_kb = _calculate_installed_size_kb(pkg_root / "usr")

        control_content = f"""Package: {meta.pkg_name_deb}
Version: {meta.version}-{meta.release}
Section: utils
Priority: optional
Architecture: {meta.deb_arch}
Installed-Size: {size_kb}
Maintainer: {meta.maintainer}
Homepage: {meta.url}
Description: {meta.summary}
 {meta.description}
Depends: libc6, libfreetype6, libfontconfig1, libharfbuzz0b, libwayland-client0, libxkbcommon0
"""
        (debian_dir / "control").write_text(control_content, encoding="utf-8")

        dpkg_deb = shutil.which("dpkg-deb")
        if dpkg_deb:
            subprocess.check_call(
                [
                    dpkg_deb,
                    "--build",
                    "--root-owner-group",
                    str(pkg_root),
                    str(output_path),
                ],
                stdout=subprocess.DEVNULL,
            )
        else:
            _build_deb_fallback(pkg_root, output_path)

    return (
        output_path,
        f"sudo apt install ./{output_path}  # or sudo dpkg -i {output_path}",
    )
