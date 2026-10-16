#!/usr/bin/env python3
"""Unified post-installation driver for wsap_term."""

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import pwd
except ImportError:
    pwd = None

@dataclass(frozen=True)
class InstallContext:
    """Environment, paths, and user context for post-installation."""

    root: Path
    destdir: str
    prefix: Path
    install_root: Path
    uid: int | None = None
    gid: int | None = None
    home: Path = field(default_factory=Path.home)

    @classmethod
    def detect(cls) -> "InstallContext":
        """Detect environment from Meson variables and process owner."""
        meson_root = os.environ.get("MESON_SOURCE_ROOT")
        root = (
            Path(meson_root).resolve()
            if meson_root
            else Path(__file__).resolve().parents[2]
        )
        destdir = os.environ.get("DESTDIR", "")
        prefix = Path(os.environ.get("MESON_INSTALL_PREFIX", "/usr/local"))
        meson_install_root = os.environ.get("MESON_INSTALL_DESTDIR_PREFIX")
        if meson_install_root:
            install_root = Path(meson_install_root)
        elif destdir:
            install_root = Path(destdir + str(prefix))
        else:
            install_root = prefix

        uid, gid, home = None, None, Path.home()
        sudo_user = os.environ.get("SUDO_USER")
        if sudo_user and pwd:
            with contextlib.suppress(KeyError):
                ent = pwd.getpwnam(sudo_user)
                uid, gid, home = ent.pw_uid, ent.pw_gid, Path(ent.pw_dir)

        return cls(
            root=root,
            destdir=destdir,
            prefix=prefix,
            install_root=install_root,
            uid=uid,
            gid=gid,
            home=home,
        )

    def install_path(self, path: Path) -> Path:
        """Map an install path through Meson's DESTDIR staging prefix."""
        try:
            relative = path.relative_to(self.prefix)
        except ValueError:
            if self.destdir:
                return Path(self.destdir + str(path))
            return path
        return self.install_root / relative

    def chown_tree(self, path: Path) -> None:
        """Recursively chown path to the original invoking user under sudo."""
        if (
            self.uid is None
            or self.gid is None
            or not hasattr(os, "chown")
            or not path.exists()
        ):
            return
        targets = [path, *path.rglob("*")] if path.is_dir() else [path]
        for item in targets:
            with contextlib.suppress(OSError):
                os.chown(item, self.uid, self.gid)

    def themes_dir(self) -> Path:
        """User themes directory."""
        if self.uid is not None:
            return self.home / ".config" / "wsap_term" / "themes"
        xdg = os.environ.get("XDG_CONFIG_HOME")
        if xdg:
            return Path(xdg) / "wsap_term" / "themes"
        return self.home / ".config" / "wsap_term" / "themes"


def _compile_terminfo(ctx: InstallContext, source: Path, dest: Path) -> bool:
    """Compile source into dest with tools/install-terminfo.sh."""
    script = ctx.root / "tools" / "install-terminfo.sh"
    env = dict(
        os.environ,
        WSAP_TERMINFO_SRC=str(source),
        WSAP_TERMINFO_DEST=str(dest),
        WSAP_TERMINFO_STALE="",
    )
    try:
        subprocess.check_call(["sh", str(script)], env=env)
    except (subprocess.CalledProcessError, OSError) as e:
        sys.stderr.write(f"Failed to compile terminfo into {dest}: {e}\n")
        return False
    return True


def install_terminfo(
    ctx: InstallContext,
    source: Path | None = None,
    *,
    datadir: Path = Path("share"),
    bindir: Path = Path("bin"),
) -> int:
    """Compile and install the terminfo entry."""
    source = source or ctx.root / "assets" / "terminfo" / "wsap_term.terminfo"
    if not source.is_file():
        sys.stderr.write(f"Terminfo file not found: {source}\n")
        return 1

    if not shutil.which("tic"):
        sys.stderr.write("tic not found; skipping terminfo installation\n")
        return 0

    data_root = datadir if datadir.is_absolute() else ctx.prefix / datadir
    dest = ctx.install_path(data_root / "terminfo")
    if not _compile_terminfo(ctx, source, dest):
        return 0

    if ctx.destdir or _is_root():
        return 0
    installer = ctx.prefix / bindir / "install-terminfo.sh"
    sys.stdout.write(
        "To also use wsap_term terminfo as root, run:\n"
        f"    sudo {installer}\n"
    )
    return 0


def _is_root() -> bool:
    return hasattr(os, "geteuid") and os.geteuid() == 0


def install_themes(ctx: InstallContext, source_dir: Path | None = None) -> int:
    """Install default themes into user config directory."""
    if ctx.destdir:
        return 0
    source_dir = source_dir or ctx.root / "assets" / "themes"
    if not source_dir.is_dir():
        sys.stderr.write(f"Themes directory not found: {source_dir}\n")
        return 0

    dest = ctx.themes_dir()
    dest.mkdir(parents=True, exist_ok=True)
    for f in source_dir.iterdir():
        if f.is_file() and f.name.endswith((".css", ".conf")):
            target = dest / f.name
            if not target.exists():
                shutil.copy2(f, target)
    ctx.chown_tree(dest)
    sys.stdout.write(f"Installed themes to {dest}\n")
    return 0


def sign_macos_bundle(ctx: InstallContext, app_dir: Path) -> int:
    """Sign nested libraries and seal the installed app."""
    bundle = app_dir / "wsap_term.app"
    if not bundle.is_absolute():
        bundle = ctx.prefix / bundle
    bundle = ctx.install_path(bundle)
    codesign = shutil.which("codesign")
    if not codesign:
        sys.stderr.write("codesign not found; cannot sign the installed app\n")
        return 1
    contents = bundle / "Contents"
    libraries = sorted((contents / "Frameworks").glob("*.dylib"))
    libraries += sorted((contents / "MacOS").glob("*.dylib"))
    try:
        for target in [*libraries, bundle]:
            subprocess.check_call([codesign, "--force", "--sign", "-", str(target)])
    except (OSError, subprocess.CalledProcessError) as error:
        sys.stderr.write(f"Cannot sign the installed app: {error}\n")
        return 1
    sys.stdout.write(f"Signed app bundle at {bundle}\n")
    return 0


def main() -> int:
    """Run post-installation tasks."""
    parser = argparse.ArgumentParser(description="Post-installation driver")
    parser.add_argument(
        "subcommand",
        nargs="?",
        default="all",
        choices=["all", "terminfo", "themes"],
    )
    parser.add_argument("--datadir", type=Path, default=Path("share"))
    parser.add_argument("--bindir", type=Path, default=Path("bin"))
    parser.add_argument("--macos-app-dir", type=Path)
    args = parser.parse_args()

    ctx = InstallContext.detect()
    if args.subcommand == "all":
        status = install_terminfo(ctx, datadir=args.datadir, bindir=args.bindir)
        status |= install_themes(ctx)
        if args.macos_app_dir is not None:
            status |= sign_macos_bundle(ctx, args.macos_app_dir)
        return status
    if args.subcommand == "terminfo":
        return install_terminfo(ctx, datadir=args.datadir, bindir=args.bindir)
    if args.subcommand == "themes":
        return install_themes(ctx)
    return 0


if __name__ == "__main__":
    sys.exit(main())
