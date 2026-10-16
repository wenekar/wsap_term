#!/usr/bin/env python3

import argparse
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

_deploy_dir = Path(__file__).resolve().parent
if str(_deploy_dir) not in sys.path:
    sys.path.insert(0, str(_deploy_dir))

from arch import build_package as build_arch
from common import (
    PackageMetadata,
    get_repo_root,
    stage_linux_payload,
)
from debian import build_package as build_debian
from fedora import build_package as build_fedora
from macos import build_package as build_macos

def build_generic_tarball(
    repo_root: Path, dist_dir: Path, meta: PackageMetadata
) -> tuple[Path, str]:
    dist_dir.mkdir(parents=True, exist_ok=True)
    pkg_name = f"{meta.name}-{meta.version}-linux-{meta.arch}.tar.gz"
    output_path = dist_dir / pkg_name

    with tempfile.TemporaryDirectory(prefix="wsap-tar-") as tmp:
        pkg_root = Path(tmp) / f"{meta.name}-{meta.version}"
        stage_linux_payload(repo_root, pkg_root / "usr")

        install_sh = pkg_root / "install.sh"
        install_sh.write_text(
            """#!/bin/sh
set -e
PREFIX="${PREFIX:-/usr/local}"
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Installing wsap_term into $PREFIX..."
mkdir -p "$PREFIX/bin" "$PREFIX/share"
cp -r "$DIR/usr/bin"/* "$PREFIX/bin/"
cp -r "$DIR/usr/share"/* "$PREFIX/share/"
echo "Successfully installed wsap_term!"
""",
            encoding="utf-8",
        )
        install_sh.chmod(0o755)

        with tarfile.open(output_path, "w:gz") as tar:
            tar.add(pkg_root, arcname=pkg_root.name)

    return (
        output_path,
        f"tar -xzf {output_path.name} && sudo ./{pkg_root.name}/install.sh",
    )

def main() -> int:
    parser = argparse.ArgumentParser(description="Build native OS deployment package")
    parser.add_argument(
        "--format",
        required=True,
        choices=["arch", "deb", "rpm", "app", "tar"],
        help="Package format specified by the build system",
    )
    parser.add_argument(
        "--version",
        required=True,
        help="Project version from Meson",
    )
    parser.add_argument(
        "--license",
        required=True,
        help="Project license from Meson",
    )
    parser.add_argument(
        "--dist-dir",
        type=Path,
        default=None,
        help="Destination directory for built packages (default: <repo>/dist)",
    )
    args = parser.parse_args()

    repo_root = get_repo_root()
    dist_dir = args.dist_dir or (repo_root / "dist")
    meta = PackageMetadata(version=args.version, license=args.license)

    builders = {
        "arch": build_arch,
        "deb": build_debian,
        "rpm": build_fedora,
        "app": build_macos,
        "tar": build_generic_tarball,
    }
    builder = builders[args.format]

    print(f"==> Packaging wsap_term {meta.version} [{args.format}] ({meta.arch})...")

    try:
        pkg_path, cmd = builder(repo_root, dist_dir, meta)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        sys.stderr.write(f"Packaging failed: {e}\n")
        return 1

    print(f"==> Package ready: {pkg_path}")
    print(f"    Install with: {cmd}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
