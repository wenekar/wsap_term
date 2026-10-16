
import contextlib
import os
import platform
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

@dataclass(frozen=True)
class PackageMetadata:

    version: str
    license: str
    name: str = "wsap_term"
    pkg_name_deb: str = "wsap-term"
    release: str = "1"
    summary: str = "Lightning-fast, lightweight terminal emulator"
    description: str = (
        "Blistering quick, minimal-latency terminal emulator featuring "
        "SIMD-accelerated text rendering, fast PTY transport, and near zero latency."
    )
    url: str = "https://github.com/wenekar/wsap_term"
    maintainer: str = "wenekar"

    @property
    def arch(self) -> str:
        m = platform.machine().lower()
        if m in ("x86_64", "amd64"):
            return "x86_64"
        if m in ("aarch64", "arm64"):
            return "aarch64"
        return m

    @property
    def deb_arch(self) -> str:
        if self.arch == "x86_64":
            return "amd64"
        if self.arch == "aarch64":
            return "arm64"
        return self.arch

def get_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]

def find_built_binary(repo_root: Path, name: str) -> Path:
    meson_build = os.environ.get("MESON_BUILD_ROOT")
    build_dir = Path(meson_build) if meson_build else repo_root / "build"
    binary = build_dir / "bin" / name
    if binary.is_file() and os.access(binary, os.X_OK):
        return binary
    raise FileNotFoundError(
        f"Required binary '{name}' not found at {binary}. Run 'make' or 'meson compile' first."
    )

def compile_terminfo(repo_root: Path, dest: Path) -> None:
    env = dict(
        os.environ,
        WSAP_TERMINFO_SRC=str(repo_root / "assets" / "terminfo" / "wsap_term.terminfo"),
        WSAP_TERMINFO_DEST=str(dest),
        WSAP_TERMINFO_STALE="",
    )
    with contextlib.suppress(OSError):
        subprocess.run(
            ["sh", str(repo_root / "assets" / "terminfo" / "install-terminfo.sh")],
            env=env,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

def stage_linux_payload(repo_root: Path, staging_usr: Path) -> None:
    bin_dir = staging_usr / "bin"
    share_dir = staging_usr / "share"
    bin_dir.mkdir(parents=True, exist_ok=True)
    share_dir.mkdir(parents=True, exist_ok=True)

    license_dir = share_dir / "licenses" / "wsap_term"
    license_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo_root / "LICENSE", license_dir / "LICENSE")

    wsap_term_bin = find_built_binary(repo_root, "wsap_term")
    shutil.copy2(wsap_term_bin, bin_dir / "wsap_term")

    with contextlib.suppress(FileNotFoundError):
        libfastpty = find_built_binary(repo_root, "libfastpty.so")
        shutil.copy2(libfastpty, bin_dir / "libfastpty.so")

    wsap_ssh = repo_root / "assets" / "shell" / "wsap-ssh"
    if wsap_ssh.is_file():
        shutil.copy2(wsap_ssh, bin_dir / "wsap-ssh")

    apps_dir = share_dir / "applications"
    apps_dir.mkdir(parents=True, exist_ok=True)
    desktop_src = repo_root / "assets" / "wsap_term.desktop"
    if desktop_src.is_file():
        shutil.copy2(desktop_src, apps_dir / "wsap_term.desktop")

    icons_dir = share_dir / "icons" / "hicolor" / "scalable" / "apps"
    icons_dir.mkdir(parents=True, exist_ok=True)
    icon_src = repo_root / "assets" / "icons" / "wsap-term.svg"
    if icon_src.is_file():
        shutil.copy2(icon_src, icons_dir / "wsap-term.svg")

    themes_src = repo_root / "assets" / "themes"
    if themes_src.is_dir():
        themes_dest = share_dir / "wsap_term" / "themes"
        themes_dest.mkdir(parents=True, exist_ok=True)
        for f in themes_src.iterdir():
            if f.is_file() and f.name.endswith((".css", ".conf")):
                shutil.copy2(f, themes_dest / f.name)

    shell_src = repo_root / "assets" / "shell"
    if shell_src.is_dir():
        shell_dest = share_dir / "wsap_term" / "shell"
        shell_dest.mkdir(parents=True, exist_ok=True)
        for f in (shell_src / "wsap-ssh.sh", shell_src / "wsap-ssh.fish"):
            if f.is_file():
                shutil.copy2(f, shell_dest / f.name)

    compile_terminfo(repo_root, share_dir / "terminfo")
