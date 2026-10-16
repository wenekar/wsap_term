#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path

VERSION_PATTERNS = (
    re.compile(r"^#\s*[A-Za-z-]+-(\d+\.\d+(?:\.\d+)?)\.txt\s*$"),
    re.compile(r"^#\s*Version:\s*(\d+\.\d+(?:\.\d+)?)\s*$"),
    re.compile(r"^#\s*Used with Emoji Version (\d+\.\d+(?:\.\d+)?)\b"),
)

def merge(values):
    values = sorted(set(values))
    ranges = []
    for cp in values:
        if ranges and cp == ranges[-1][1] + 1:
            ranges[-1] = (ranges[-1][0], cp)
        else:
            ranges.append((cp, cp))
    return ranges

def field_ranges(path, wanted):
    values = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        span, prop = (part.strip() for part in line.split(";", 1))
        if prop not in wanted:
            continue
        ends = [int(x, 16) for x in span.split("..")]
        values.extend(range(ends[0], ends[-1] + 1))
    return merge(values)

def normalize_version(version):
    parts = version.split(".")
    return ".".join(parts + ["0"] * (3 - len(parts)))

def header_version(path):
    found = set()
    for raw in path.read_text().splitlines():
        if not raw.startswith("#"):
            break
        for pattern in VERSION_PATTERNS:
            match = pattern.match(raw)
            if match:
                found.add(normalize_version(match.group(1)))
    if len(found) != 1:
        sys.exit(f"{path}: expected one Unicode version in the header, found {sorted(found) or 'none'}")
    return found.pop()

def unicode_categories(path):
    by_category = {}
    pending = None
    for raw in path.read_text().splitlines():
        fields = raw.split(";")
        cp, name, category = int(fields[0], 16), fields[1], fields[2]
        if name.endswith(", First>"):
            pending = (cp, category)
            continue
        if name.endswith(", Last>"):
            start, first_category = pending
            assert category == first_category
            by_category.setdefault(category, []).extend(range(start, cp + 1))
            pending = None
        else:
            by_category.setdefault(category, []).append(cp)
    assert pending is None
    return by_category

def emit_table(out, name, ranges):
    out.append(f"static const struct wsap_vtcore_unicode_range {name}[] = {{")
    for i in range(0, len(ranges), 4):
        chunk = ranges[i:i + 4]
        out.append("\t" + ",".join(f"{{0x{lo:X},0x{hi:X}}}" for lo, hi in chunk) + ",")
    out.append("};\n")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("unicode_data", type=Path)
    parser.add_argument("east_asian_width", type=Path)
    parser.add_argument("emoji_data", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    versions = {path: header_version(path) for path in (args.east_asian_width, args.emoji_data)}
    if len(set(versions.values())) != 1:
        sys.exit("input files disagree on the Unicode version: " +
                 ", ".join(f"{path}={version}" for path, version in versions.items()))
    version = next(iter(versions.values()))

    categories = unicode_categories(args.unicode_data)
    assigned = {cp for cps in categories.values() for cp in cps}
    jamo = list(range(0x1160, 0x1200)) + [cp for cp in range(0xD7B0, 0xD800) if cp in assigned]
    zero = merge(categories.get("Mn", []) + categories.get("Me", []) +
                 categories.get("Cf", []) + jamo)
    nonprint = merge(categories.get("Cc", []) + categories.get("Cs", []))
    wide = field_ranges(args.east_asian_width, {"W", "F"})
    spacing = merge(categories.get("Mc", []))
    pictographic = field_ranges(args.emoji_data, {"Extended_Pictographic"})
    emoji_presentation = field_ranges(args.emoji_data, {"Emoji_Presentation"})

    out = [f"/* Generated from Unicode {version} data by tools/generate_unicode.py. */", "",
           f'#define WSAP_VTCORE_UNICODE_DATA_VERSION "{version}"', ""]
    emit_table(out, "wsap_vtcore_zero_width", zero)
    emit_table(out, "wsap_vtcore_nonprintable", nonprint)
    emit_table(out, "wsap_vtcore_unicode_wide", wide)
    emit_table(out, "wsap_vtcore_spacing_mark", spacing)
    emit_table(out, "wsap_vtcore_extended_pictographic", pictographic)
    emit_table(out, "wsap_vtcore_emoji_presentation", emoji_presentation)
    args.output.write_text("\n".join(out))

if __name__ == "__main__":
    main()
