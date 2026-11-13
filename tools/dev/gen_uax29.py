#!/usr/bin/env python3
"""Generate compact 3-stage UAX #29 property lookup tables from host ICU."""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import re
import sys
from pathlib import Path

UCHAR_ALPHABETIC = 0
UCHAR_IDEOGRAPHIC = 17
UCHAR_EXTENDED_PICTOGRAPHIC = 64
UCHAR_GRAPHEME_CLUSTER_BREAK = 0x1012
UCHAR_WORD_BREAK = 0x1014
UCHAR_INDIC_CONJUNCT_BREAK = 0x101A

GCB_MAP = {
    0: ("GCB_OTHER", 0),
    1: ("GCB_CONTROL", 1),
    2: ("GCB_CR", 2),
    3: ("GCB_EXTEND", 3),
    4: ("GCB_L", 4),
    5: ("GCB_LF", 5),
    6: ("GCB_LV", 6),
    7: ("GCB_LVT", 7),
    8: ("GCB_T", 8),
    9: ("GCB_V", 9),
    10: ("GCB_SPACING_MARK", 10),
    11: ("GCB_PREPEND", 11),
    12: ("GCB_RI", 12),
    17: ("GCB_ZWJ", 13),
}

WB_MAP = {
    0: ("WB_OTHER", 0),
    1: ("WB_ALETTER", 1),
    2: ("WB_FORMAT", 2),
    3: ("WB_KATAKANA", 3),
    4: ("WB_MIDLETTER", 4),
    5: ("WB_MIDNUM", 5),
    6: ("WB_NUMERIC", 6),
    7: ("WB_EXTENDNUMLET", 7),
    8: ("WB_CR", 8),
    9: ("WB_EXTEND", 9),
    10: ("WB_LF", 10),
    11: ("WB_MIDNUMLET", 11),
    12: ("WB_NEWLINE", 12),
    13: ("WB_RI", 13),
    14: ("WB_HEBREW_LETTER", 14),
    15: ("WB_SINGLE_QUOTE", 15),
    16: ("WB_DOUBLE_QUOTE", 16),
    21: ("WB_ZWJ", 17),
    22: ("WB_WSEGSPACE", 18),
}

INCB_MAP = {
    0: ("INCB_NONE", 0),
    1: ("INCB_CONSONANT", 1),
    2: ("INCB_EXTEND", 2),
    3: ("INCB_LINKER", 3),
}

WORD_WB = {1, 3, 6, 14}
STAGE2_BLOCK = 16
STAGE1_BLOCK = 32
MARKER_BEGIN = "/* BEGIN GENERATED UAX29 TABLES */"
MARKER_END = "/* END GENERATED UAX29 TABLES */"


def load_icu(lib_path: str | None):
    candidates = [lib_path] if lib_path else [
        ctypes.util.find_library("icuuc"),
        "libicuuc.so",
        "libicuuc.dylib",
    ]
    for path in candidates:
        if not path:
            continue
        try:
            return ctypes.CDLL(path)
        except OSError:
            continue
    sys.exit("gen_uax29: could not load libicuuc")


def find_symbol(lib, name: str):
    fn = getattr(lib, name, None)
    if fn is not None:
        return fn
    for ver in range(50, 140):
        fn = getattr(lib, f"{name}_{ver}", None)
        if fn is not None:
            return fn
    sys.exit(f"gen_uax29: symbol {name} not found in libicuuc")


def build_tables(lib):
    get_prop = find_symbol(lib, "u_getIntPropertyValue")
    get_prop.argtypes = [ctypes.c_int32, ctypes.c_int]
    get_prop.restype = ctypes.c_int32

    get_ver = find_symbol(lib, "u_getUnicodeVersion")
    get_ver.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
    ver_arr = (ctypes.c_uint8 * 4)()
    get_ver(ver_arr)
    unicode_version = f"{ver_arr[0]}.{ver_arr[1]}.{ver_arr[2]}"

    unique = {}
    props_list = []
    cp_ids = []

    for cp in range(0x110000):
        raw_gcb = get_prop(cp, UCHAR_GRAPHEME_CLUSTER_BREAK)
        raw_wb = get_prop(cp, UCHAR_WORD_BREAK)
        raw_incb = get_prop(cp, UCHAR_INDIC_CONJUNCT_BREAK)
        ext_pict = 1 if get_prop(cp, UCHAR_EXTENDED_PICTOGRAPHIC) else 0
        alpha = 1 if get_prop(cp, UCHAR_ALPHABETIC) else 0
        ideo = 1 if get_prop(cp, UCHAR_IDEOGRAPHIC) else 0

        if raw_gcb not in GCB_MAP:
            sys.exit(f"gen_uax29: unsupported GCB value {raw_gcb} at U+{cp:04X}")
        if raw_wb not in WB_MAP:
            sys.exit(f"gen_uax29: unsupported WB value {raw_wb} at U+{cp:04X}")
        if raw_incb not in INCB_MAP:
            sys.exit(f"gen_uax29: unsupported InCB value {raw_incb} at U+{cp:04X}")

        gcb = GCB_MAP[raw_gcb][1]
        wb = WB_MAP[raw_wb][1]
        incb = INCB_MAP[raw_incb][1]
        is_word = 1 if (raw_wb in WORD_WB or (raw_wb == 0 and alpha) or ideo) else 0

        tup = (gcb, wb, incb, ext_pict, is_word)
        idx = unique.get(tup)
        if idx is None:
            idx = len(props_list)
            unique[tup] = idx
            props_list.append(tup)
        cp_ids.append(idx)

    assert len(props_list) <= 256

    s2_blocks = {}
    s1_raw = []
    s2 = []
    for off in range(0, 0x110000, STAGE2_BLOCK):
        blk = tuple(cp_ids[off:off + STAGE2_BLOCK])
        idx = s2_blocks.get(blk)
        if idx is None:
            idx = len(s2_blocks)
            s2_blocks[blk] = idx
            s2.extend(blk)
        s1_raw.append(idx)

    assert len(s2_blocks) <= 65536

    s1_blocks = {}
    s0 = []
    s1 = []
    for off in range(0, len(s1_raw), STAGE1_BLOCK):
        blk = tuple(s1_raw[off:off + STAGE1_BLOCK])
        idx = s1_blocks.get(blk)
        if idx is None:
            idx = len(s1_blocks)
            s1_blocks[blk] = idx
            s1.extend(blk)
        s0.append(idx)

    assert len(s1_blocks) <= 256
    return unicode_version, props_list, s0, s1, s2


def format_array(ctype: str, name: str, data: list[int], per_line: int, hex_width: int = 0) -> list[str]:
    lines = [f"static const {ctype} {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        if hex_width:
            fmt = ", ".join(f"0x{v:0{hex_width}X}u" for v in chunk)
        else:
            fmt = ", ".join(f"{v:2d}" for v in chunk)
        lines.append(f"    {fmt},")
    lines.append("};")
    return lines


def render_c(version: str, props_list, s0, s1, s2) -> str:
    gcb_by_id = {v[1]: v[0] for v in GCB_MAP.values()}
    wb_by_id = {v[1]: v[0] for v in WB_MAP.values()}
    incb_by_id = {v[1]: v[0] for v in INCB_MAP.values()}

    total_bytes = len(props_list) * 4 + len(s0) + len(s1) * 2 + len(s2)
    out = [
        MARKER_BEGIN,
        f"/* Generated from Unicode {version} by tools/dev/gen_uax29.py. */",
        f"/* Static table footprint: {total_bytes} bytes ({total_bytes / 1024.0:.2f} KiB). */",
        "",
        "enum uax29_gcb {",
    ]
    for _, (name, val) in sorted(GCB_MAP.items(), key=lambda x: x[1][1]):
        out.append(f"    {name} = {val},")
    out.extend(["};", "", "enum uax29_wb {"])
    for _, (name, val) in sorted(WB_MAP.items(), key=lambda x: x[1][1]):
        out.append(f"    {name} = {val},")
    out.extend(["    WB_NONE = 255,", "};", "", "enum uax29_incb {"])
    for _, (name, val) in sorted(INCB_MAP.items(), key=lambda x: x[1][1]):
        out.append(f"    {name} = {val},")
    out.extend([
        "};",
        "",
        "enum {",
        "    UAX29_PROP_EXT_PICT = 1u << 0,",
        "    UAX29_PROP_IS_WORD = 1u << 1,",
        "};",
        "",
        "struct uax29_prop {",
        "    uint8_t gcb;",
        "    uint8_t wb;",
        "    uint8_t incb;",
        "    uint8_t flags;",
        "};",
        "",
        f"static const struct uax29_prop uax29_props[{len(props_list)}] = {{",
    ])
    for gcb, wb, incb, ep, is_word in props_list:
        flags = []
        if ep:
            flags.append("UAX29_PROP_EXT_PICT")
        if is_word:
            flags.append("UAX29_PROP_IS_WORD")
        flag_expr = " | ".join(flags) if flags else "0"
        out.append(
            f"    {{ {gcb_by_id[gcb]}, {wb_by_id[wb]}, {incb_by_id[incb]}, {flag_expr} }},"
        )
    out.extend(["};", ""])
    out.extend(format_array("uint8_t", "uax29_stage0", s0, 16))
    out.append("")
    out.extend(format_array("uint16_t", "uax29_stage1", s1, 12, hex_width=3))
    out.append("")
    out.extend(format_array("uint8_t", "uax29_stage2", s2, 16))
    out.append(MARKER_END)
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--icu-lib", help="Path to libicuuc shared library")
    parser.add_argument("--update", type=Path, help="Update generated section inside C file")
    args = parser.parse_args()

    lib = load_icu(args.icu_lib)
    version, props_list, s0, s1, s2 = build_tables(lib)
    rendered = render_c(version, props_list, s0, s1, s2)

    if args.update:
        text = args.update.read_text()
        pattern = re.compile(
            re.escape(MARKER_BEGIN) + r".*?" + re.escape(MARKER_END),
            re.DOTALL,
        )
        if not pattern.search(text):
            sys.exit(f"gen_uax29: markers not found in {args.update}")
        updated = pattern.sub(lambda _: rendered, text, count=1)
        args.update.write_text(updated)
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
