#!/usr/bin/env python3
"""Check source include edges against the directory layer plan."""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

RANKS = {
    "util": 1,
    "platform": 1,
    "loop": 1,
    "pty": 4,
    "session": 5,
    "input": 6,
    "paint": 7,
    "font": 8,
    "ui": 9,
    "theme": 10,
    "render": 11,
    "ui/yasl": 12,
    "terminal": 13,
    "app": 14,
}

PRIVATE_OWNERS = {
    "internal.h": True,
    "debug_internal.h": True,
    "win_internal.h": True,
}
PRIVATE_EXACT = {"ui/yasl/internal.h"}
PRIVATE_ALLOW = {
    "font/glyph/internal.h": {"font/font.c"},
    "font/backends/freetype/internal.h": {"font/font.c", "font/hinting.c"},
}
LIBRARY_PATHS = {
    "base": ("util", "platform", "loop"),
    "pty": ("pty",),
    "session": ("session",),
    "input": ("input",),
    "paint": ("paint",),
    "font": ("font",),
    "ui": ("ui",),
    "theme": ("theme",),
    "render": ("render",),
    "ui_yasl": ("ui/yasl",),
    "terminal": ("terminal",),
    "app": ("app",),
}
ALLOWED_DEPENDENCIES = {
    "base": set(),
    "pty": {"base"},
    "session": {"pty", "base"},
    "input": {"base"},
    "paint": {"base"},
    "font": {"paint", "base"},
    "ui": {"paint", "input", "base"},
    "theme": {"ui", "base"},
    "render": {"paint", "font", "base"},
    "ui_yasl": {"paint", "input", "ui", "font", "base"},
    "terminal": {"ui_yasl", "session", "ui", "input", "font", "paint", "base"},
    "app": {"base", "pty", "session", "input", "paint", "font", "ui", "theme", "render", "ui_yasl", "terminal"},
}
YASL_LIBRARIES = {"base", "pty", "session", "ui_yasl", "terminal", "app"}
YASL_PREFIX = "yasl/"
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')
SOURCE_SUFFIXES = {".c", ".h", ".m", ".mm"}

def layer_for(path: str) -> tuple[int, str] | None:
    if path == "main.c":
        return 14, "app"
    for layer, rank in sorted(RANKS.items(), key=lambda item: len(item[0]), reverse=True):
        if path == layer or path.startswith(layer + "/"):
            return rank, layer
    return None

def library_for(path: str) -> str | None:
    if path == "main.c":
        return "app"
    prefixes = sorted(
        ((prefix, library) for library, items in LIBRARY_PATHS.items() for prefix in items),
        key=lambda item: len(item[0]),
        reverse=True,
    )
    for prefix, library in prefixes:
        if path == prefix or path.startswith(prefix + "/"):
            return library
    return None

def resolve_include(source: Path, included: str, src: Path, headers: set[str]) -> str | None:
    candidates = ((source.parent / included), (src / included))
    for candidate in candidates:
        try:
            relative = candidate.resolve().relative_to(src.resolve()).as_posix()
        except ValueError:
            continue
        if relative in headers:
            return relative
    return None

def is_private_violation(source: str, target: str) -> bool:
    parts = Path(target).parts
    if Path(target).name not in PRIVATE_OWNERS and "internal" not in parts:
        return False
    if source in PRIVATE_ALLOW.get(target, set()):
        return False
    if target in PRIVATE_EXACT:
        return Path(source).parent.as_posix() != Path(target).parent.as_posix()
    if "internal" in parts:
        owner = "/".join(parts[:parts.index("internal")])
    else:
        owner = Path(target).parent.as_posix()
    return not (source == owner or source.startswith(owner + "/"))

def scan(src: Path) -> list[str]:
    errors: list[str] = []
    headers = {p.relative_to(src).as_posix() for p in src.rglob("*") if p.is_file()}
    for source in sorted(p for p in src.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES):
        relative = source.relative_to(src).as_posix()
        source_layer = layer_for(relative)
        if source_layer is None:
            continue
        source_rank, source_owner = source_layer
        for number, line in enumerate(source.read_text(errors="replace").splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            if match.group(1).startswith(YASL_PREFIX) and library_for(relative) not in YASL_LIBRARIES:
                owner = library_for(relative) or source_owner
                errors.append(f"{relative}:{number}: {owner} may not include yasl: {match.group(1)}")
            included = resolve_include(source, match.group(1), src, headers)
            if included is None:
                continue
            target = layer_for(included)
            if target and target[0] > source_rank:
                errors.append(
                    f"{relative}:{number}: rank {source_rank} ({source_owner}) includes "
                    f"rank {target[0]} ({target[1]}): {included}"
                )
            source_library = library_for(relative)
            target_library = library_for(included)
            if source_library and target_library and source_library != target_library and target_library not in ALLOWED_DEPENDENCIES[source_library]:
                errors.append(f"{relative}:{number}: {source_library} may not use {target_library}: {included}")
            if is_private_violation(relative, included):
                errors.append(f"{relative}:{number}: includes private header {included}")
    return errors

def scan_private_owner(src: Path, owner: str) -> list[str]:
    errors: list[str] = []
    headers = {p.relative_to(src).as_posix() for p in src.rglob("*") if p.is_file()}
    for source in sorted(p for p in src.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES):
        relative = source.relative_to(src).as_posix()
        for number, line in enumerate(source.read_text(errors="replace").splitlines(), 1):
            match = INCLUDE.match(line)
            included = resolve_include(source, match.group(1), src, headers) if match else None
            if included and included.startswith(owner + "/") and is_private_violation(relative, included):
                errors.append(f"{relative}:{number}: includes private header {included}")
    return errors

def find_cycles(src: Path) -> list[str]:
    headers = {p.relative_to(src).as_posix() for p in src.rglob("*") if p.is_file()}
    graph: dict[str, set[str]] = {}
    for source in sorted(p for p in src.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES):
        relative = source.relative_to(src).as_posix()
        source_layer = layer_for(relative)
        if source_layer is None:
            continue
        for line in source.read_text(errors="replace").splitlines():
            match = INCLUDE.match(line)
            included = resolve_include(source, match.group(1), src, headers) if match else None
            source_library = library_for(relative)
            target_library = library_for(included) if included else None
            if source_library and target_library and target_library != source_library:
                graph.setdefault(source_library, set()).add(target_library)
    found: set[tuple[str, ...]] = set()
    active: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in active:
            cycle = active[active.index(node):]
            rotations = [tuple(cycle[i:] + cycle[:i]) for i in range(len(cycle))]
            found.add(min(rotations))
            return
        if node in visited:
            return
        active.append(node)
        for next_node in sorted(graph.get(node, ())):
            visit(next_node)
        active.pop()
        visited.add(node)

    for node in sorted(graph):
        visit(node)
    return [" -> ".join(cycle + (cycle[0],)) for cycle in sorted(found)]

def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="dirgraph-") as temp:
        src = Path(temp) / "src"
        (src / "pty").mkdir(parents=True)
        (src / "ui").mkdir()
        (src / "ui" / "screen.c").write_text('#include "pty/internal.h"\n')
        (src / "pty" / "internal.h").write_text("\n")
        (src / "pty" / "bad.c").write_text('#include "ui/public.h"\n')
        (src / "ui" / "public.h").write_text('#include "pty/internal.h"\n')
        errors = scan(src)
        cycles = find_cycles(src)
        if not any("private header pty/internal.h" in error for error in errors):
            print("self-test failed: internal.h was not treated as private", file=sys.stderr)
            return 1
        if not any("rank 4 (pty) includes rank 9 (ui)" in error for error in errors):
            print("self-test failed: rank violation was not rejected", file=sys.stderr)
            return 1
        if not cycles:
            print("self-test failed: include cycle was not rejected", file=sys.stderr)
            return 1
        (src / "ui" / "yasl_user.h").write_text('#include "yasl/yasl.h"\n')
        (src / "ui" / "yasl").mkdir()
        (src / "ui" / "yasl" / "native.c").write_text('#include "yasl/loop.h"\n')
        errors = scan(src)
        if not any("ui may not include yasl: yasl/yasl.h" in error for error in errors):
            print("self-test failed: yasl include outside the binding was not rejected", file=sys.stderr)
            return 1
        if any("ui/yasl/native.c" in error for error in errors):
            print("self-test failed: ui_yasl may not include yasl", file=sys.stderr)
            return 1
        for directory in ("ui/yasl", "app"):
            (src / directory).mkdir(parents=True, exist_ok=True)
        (src / "ui/yasl/internal.h").write_text("\n")
        (src / "ui/yasl/debug_internal.h").write_text("\n")
        (src / "ui/yasl/own.c").write_text('#include "debug_internal.h"\n')
        (src / "app/private.c").write_text(
            '#include "../ui/yasl/debug_internal.h"\n'
            '#include "ui/yasl/internal.h"\n'
        )
        errors = scan_private_owner(src, "ui")
        if len(errors) != 2 or any("own.c" in error for error in errors):
            print("self-test failed: ui_yasl private owners were not enforced", file=sys.stderr)
            return 1
    print("self-test passed: private headers, rank violations, and yasl includes are rejected")
    return 0

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", default=".", help="source tree or Meson build directory")
    parser.add_argument("--self-test", action="store_true", help="test the private-header and rank gates")
    parser.add_argument("--private-owner", help="check private headers for one source owner")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = Path(args.path).resolve()
    if (root / "build.ninja").is_file() and (root.parent / "src").is_dir():
        root = root.parent
    elif not (root / "src").is_dir() and (root.parent / "src").is_dir():
        root = root.parent
    src = root / "src"
    if not src.is_dir():
        print(f"dirgraph: no src directory under {root}", file=sys.stderr)
        return 2
    if args.private_owner:
        errors = scan_private_owner(src, args.private_owner)
        for error in errors:
            print(error)
        return bool(errors)
    errors = scan(src)
    cycles = find_cycles(src)
    if errors:
        print("Layer violations:")
        print("\n".join(errors))
    else:
        print("Layer violations: 0")
    print(f"Cycles: {len(cycles)}")
    for cycle in cycles:
        print(f"Cycle: {cycle}")
    return 1 if errors or cycles else 0

if __name__ == "__main__":
    raise SystemExit(main())
