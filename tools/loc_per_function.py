#!/usr/bin/env python3

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

@dataclass
class FuncLOC:
    file: str
    name: str
    start_line: int
    end_line: int
    total_lines: int
    code_lines: int

def _strip_comments_and_strings(src: str) -> str:
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n:
                if src[i] == "*" and i + 1 < n and src[i + 1] == "/":
                    out.append("  ")
                    i += 2
                    break
                if src[i] == "\n":
                    out.append("\n")
                else:
                    out.append(" ")
                i += 1
            continue
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if src[i] == '"':
                    out.append('"')
                    i += 1
                    break
                if src[i] == "\n":
                    out.append("\n")
                else:
                    out.append(" ")
                i += 1
            continue
        if c == "'":
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                if src[i] == "'":
                    out.append("'")
                    i += 1
                    break
                if src[i] == "\n":
                    out.append("\n")
                else:
                    out.append(" ")
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)

def _find_matching_brace(src: str, open_pos: int) -> int | None:
    depth = 0
    i = open_pos
    n = len(src)
    while i < n:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None

def _line_of(pos: int, src: str) -> int:
    return src[:pos].count("\n") + 1

def _code_lines(raw_lines: list[str], start: int, end: int) -> int:
    count = 0
    in_block = False
    for lineno in range(start, end + 1):
        if lineno - 1 >= len(raw_lines):
            break
        line = raw_lines[lineno - 1]
        stripped = line.strip()

        if in_block:
            if "*/" in stripped:
                in_block = False
                after = stripped[stripped.index("*/") + 2 :]
                if after and not after.startswith("//"):
                    count += 1
            continue

        if stripped.startswith("/*") and "*/" not in stripped:
            in_block = True
            before = stripped[: stripped.index("/*")]
            if before:
                count += 1
            continue

        if stripped.startswith("//"):
            continue
        if not stripped:
            continue
        count += 1
    return count

def _is_likely_func_def(lines: list[str], idx: int) -> tuple[str, int] | None:
    line = lines[idx]
    if not line or line[0] in ("#", "\t", " ", "}", "/"):
        return None

    first_word = line.split("(")[0].split()
    if not first_word:
        return None
    if first_word[0] in ("if", "for", "while", "switch", "do", "else", "return", "typedef"):
        return None
    if first_word[0] in ("struct", "enum", "union") and "{" in line and "(" not in line:
        return None

    sig = ""
    for j in range(idx, min(idx + 10, len(lines))):
        sig += lines[j] + " "
        semi = sig.find(";")
        brace = sig.find("{")
        if brace != -1 and (semi == -1 or brace < semi):
            before_paren = sig[: sig.find("(")]
            before_paren = re.sub(r"__attribute__\s*\(\([^)]*\)\)", "", before_paren)
            name_match = re.search(r"(\*?)(\w+)\s*$", before_paren.strip())
            if name_match:
                name = name_match.group(2)
                if name == "__attribute__":
                    return None
                return (name, j)
            return None
        if semi != -1 and (brace == -1 or semi < brace):
            return None
    return None

def collect_loc(root: str) -> list[FuncLOC]:
    all_funcs: list[FuncLOC] = []
    skip_dirs = {"extern", "__pycache__", "venv", ".git"}

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".") and d not in skip_dirs]
        for fname in filenames:
            if not (fname.endswith(".c") or fname.endswith(".h")):
                continue
            filepath = os.path.join(dirpath, fname)
            try:
                with open(filepath, encoding="utf-8", errors="replace") as f:
                    raw_src = f.read()
            except OSError:
                continue

            raw_lines = raw_src.splitlines()
            stripped_src = _strip_comments_and_strings(raw_src)
            stripped_lines = stripped_src.splitlines()

            idx = 0
            while idx < len(stripped_lines):
                result = _is_likely_func_def(stripped_lines, idx)
                if result is None:
                    idx += 1
                    continue

                name, brace_line_idx = result

                line_start = 0
                for li in range(brace_line_idx):
                    line_start += len(stripped_lines[li]) + 1

                brace_pos = stripped_src.find("{", line_start)
                if brace_pos == -1:
                    idx += 1
                    continue

                close_pos = _find_matching_brace(stripped_src, brace_pos)
                if close_pos is None:
                    idx += 1
                    continue

                start_line = idx + 1
                end_line = _line_of(close_pos, stripped_src)

                if end_line < start_line:
                    idx += 1
                    continue

                total = end_line - start_line + 1
                code = _code_lines(raw_lines, start_line, end_line)

                all_funcs.append(
                    FuncLOC(
                        file=filepath,
                        name=name,
                        start_line=start_line,
                        end_line=end_line,
                        total_lines=total,
                        code_lines=code,
                    )
                )
                idx = end_line
    return all_funcs

def main() -> None:
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    funcs = collect_loc(root)
    funcs.sort(key=lambda f: f.code_lines, reverse=True)

    cw_file = max((len(f.file) for f in funcs), default=10)
    cw_name = max((len(f.name) for f in funcs), default=10)

    header = f"{'File':<{cw_file}}  {'Function':<{cw_name}}  {'Start':>5}  {'End':>5}  {'Total':>5}  {'Code':>4}"
    print(header)
    print("-" * len(header))

    total_code = 0
    for f in funcs:
        print(
            f"{f.file:<{cw_file}}  {f.name:<{cw_name}}  {f.start_line:>5}  {f.end_line:>5}  {f.total_lines:>5}  {f.code_lines:>4}"
        )
        total_code += f.code_lines

    print("-" * len(header))
    print(f"Total functions: {len(funcs)}   Total code lines: {total_code}")

if __name__ == "__main__":
    main()
