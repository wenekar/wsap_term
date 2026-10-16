
import contextlib
import os
import shutil
import stat
import subprocess
from pathlib import Path

from common import PackageMetadata, compile_terminfo, find_built_binary

def _built_info_plist(repo_root: Path) -> Path:
    meson_build = os.environ.get("MESON_BUILD_ROOT")
    build_dir = Path(meson_build) if meson_build else repo_root / "build"
    plist = build_dir / "Info.plist"
    if plist.is_file():
        return plist
    raise FileNotFoundError(
        f"Info.plist not found at {plist}. Run 'meson setup' on macOS first."
    )

def _bundle_frameworks(binary: Path, frameworks_dir: Path) -> None:
    otool = shutil.which("otool")
    install_name_tool = shutil.which("install_name_tool")
    if not otool or not install_name_tool or not binary.is_file():
        return

    frameworks_dir.mkdir(parents=True, exist_ok=True)
    queue = [binary]
    seen: set[str] = set()
    copied: dict[str, Path] = {}

    while queue:
        curr = queue.pop(0)
        try:
            out = subprocess.check_output([otool, "-L", str(curr)], text=True)
        except (subprocess.CalledProcessError, OSError):
            continue

        for line in out.splitlines()[1:]:
            entry = line.strip().split(" (compatibility")[0].strip()
            if not entry or entry.startswith(
                ("@rpath", "@executable_path", "/System/", "/usr/lib/")
            ):
                continue
            lib_path = Path(entry)
            if not lib_path.is_file():
                continue

            name = lib_path.name
            if name not in seen:
                seen.add(name)
                dest = frameworks_dir / name
                shutil.copy2(lib_path, dest)
                dest.chmod(dest.stat().st_mode | stat.S_IWUSR)
                copied[str(lib_path)] = dest
                queue.append(dest)

    for old_path_str, bundled_lib in copied.items():
        lib_name = bundled_lib.name
        subprocess.run(
            [install_name_tool, "-id", f"@rpath/{lib_name}", str(bundled_lib)],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for target in [binary, *copied.values()]:
            subprocess.run(
                [
                    install_name_tool,
                    "-change",
                    old_path_str,
                    f"@rpath/{lib_name}",
                    str(target),
                ],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    subprocess.run(
        [
            install_name_tool,
            "-add_rpath",
            "@executable_path/../Frameworks",
            str(binary),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

def _sign(bundle_dir: Path) -> None:
    codesign = shutil.which("codesign")
    if not codesign:
        return
    contents = bundle_dir / "Contents"
    nested = sorted((contents / "Frameworks").glob("*.dylib"))
    nested += sorted((contents / "MacOS").glob("*.dylib"))
    for target in [*nested, bundle_dir]:
        subprocess.check_call(
            [codesign, "--force", "--sign", "-", str(target)],
            stdout=subprocess.DEVNULL,
        )

def build_package(
    repo_root: Path, dist_dir: Path, meta: PackageMetadata
) -> tuple[Path, str]:
    dist_dir.mkdir(parents=True, exist_ok=True)
    app_dir = dist_dir / "wsap_term.app"
    if app_dir.exists():
        shutil.rmtree(app_dir)

    contents = app_dir / "Contents"
    macos_dir = contents / "MacOS"
    resources_dir = contents / "Resources"
    macos_dir.mkdir(parents=True, exist_ok=True)
    resources_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo_root / "LICENSE", resources_dir / "LICENSE")

    target_bin = find_built_binary(repo_root, "wsap_term")
    dest_bin = macos_dir / "wsap_term"
    shutil.copy2(target_bin, dest_bin)
    dest_bin.chmod(dest_bin.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    with contextlib.suppress(FileNotFoundError):
        libfastpty = find_built_binary(repo_root, "libfastpty.dylib")
        shutil.copy2(libfastpty, macos_dir / "libfastpty.dylib")

    icon = repo_root / "assets" / "icons" / "wsap-term.icns"
    if icon.is_file():
        shutil.copy2(icon, resources_dir / "AppIcon.icns")

    themes = repo_root / "assets" / "themes"
    if themes.is_dir():
        shutil.copytree(themes, resources_dir / "themes", dirs_exist_ok=True)

    fonts = repo_root / "assets" / "fonts"
    if fonts.is_dir():
        shutil.copytree(fonts, resources_dir / "fonts", dirs_exist_ok=True)

    shell = repo_root / "assets" / "shell"
    if shell.is_dir():
        shutil.copytree(shell, resources_dir / "shell", dirs_exist_ok=True)

    compile_terminfo(repo_root, resources_dir / "terminfo")

    shutil.copy2(_built_info_plist(repo_root), contents / "Info.plist")

    _bundle_frameworks(dest_bin, contents / "Frameworks")
    _sign(app_dir)

    hdiutil = shutil.which("hdiutil")
    if hdiutil:
        dmg_path = dist_dir / f"wsap_term-{meta.version}-macos.dmg"
        if dmg_path.exists():
            dmg_path.unlink()
        subprocess.check_call(
            [
                hdiutil,
                "create",
                "-volname",
                "wsap_term",
                "-srcfolder",
                str(app_dir),
                "-ov",
                "-format",
                "UDZO",
                str(dmg_path),
            ],
            stdout=subprocess.DEVNULL,
        )
        return (
            dmg_path,
            f"Open {dmg_path.name} and drag wsap_term.app into /Applications",
        )

    return app_dir, f"Drag {app_dir.name} into /Applications"
