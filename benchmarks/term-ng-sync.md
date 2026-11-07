# Terminal core: the wsap_term-ng fast paths

wsap_term-ng edited its copy of the core amalgamation by hand. This pass
brought those edits back to `subprojects/term`, fixed their faults, and kept
only the fast paths with a measured gain. It also made the snapshot row
store a ring. `subprojects/term/docs/correctness-audit.md` lists the faults.

## Measurement

The CPU is an AMD Ryzen 7 9700X. The compiler is GCC 16.2.1. All builds use
the standalone core in release mode with LTO and `-Darch_flag=-march=native`.
The terminal has 120 columns, 40 rows, and 4096 history lines.

Three builds ran:

- base: commit `267ae0c`, before the ng changes.
- final: this pass.
- no fast paths: this pass with every new fast path removed. It keeps the
  fault fixes and the snapshot ring.

Final against no fast paths gives the gain of one fast path. Each new case
exercises mostly one path. Each round runs the three builds one after the
other on CPU 4, case by case, in a different order each round. Three rounds
ran. The table gives the median of the three sample medians.

```sh
taskset -c 4 term-write-bench CASE 42 0 256 5
taskset -c 4 term-snapshot-bench all 5000 5
```

`term-snapshot-bench` times `wsap_vtcore_snapshot_update` alone, after one
change to the terminal. Lower is better for its results.

A difference below 3% is noise. `lf-burst` and `crlf-burst` vary by up to
10% between runs of the same build, so the table leaves them out.

## Results

| Case | Write size | Base MiB/s | Final MiB/s | No fast paths MiB/s | Final/base | Final/no fast paths |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sgr-short | 65529 | 521 | 651 | 522 | 1.25x | 1.25x |
| sgr-short | 4096 | 514 | 621 | 515 | 1.21x | 1.20x |
| sgr-prefix | 65518 | 768 | 1228 | 769 | 1.60x | 1.60x |
| sgr-prefix | 4096 | 757 | 1200 | 748 | 1.59x | 1.60x |
| csi-cursor | 65527 | 445 | 545 | 441 | 1.22x | 1.24x |
| csi-cursor | 4096 | 442 | 544 | 442 | 1.23x | 1.23x |
| csi-modes | 65520 | 421 | 701 | 427 | 1.66x | 1.64x |
| csi-modes | 4096 | 406 | 688 | 404 | 1.70x | 1.70x |
| colors | 65527 | 717 | 784 | 722 | 1.09x | 1.09x |
| dch | 65505 | 521 | 567 | 536 | 1.09x | 1.06x |
| manyline | 4096 | 7707 | 8260 | 7653 | 1.07x | 1.08x |
| ascii | 4096 | 8567 | 8859 | 8616 | 1.03x | 1.03x |
| fg-rand | 65521 | 1918 | 1945 | 1922 | 1.01x | 1.01x |
| fgbg-char | 65439 | 2859 | 2866 | 2873 | 1.00x | 1.00x |
| cursor | 4096 | 729 | 717 | 717 | 0.98x | 1.00x |
| utf8 | 4096 | 754 | 755 | 754 | 1.00x | 1.00x |

The other cases (fg-char, fgbg-rand, erase, editor, longline, utf8var, cjk,
clusters, random) changed by less than 3%.

| Snapshot case | Base ns | Final ns | No fast paths ns | Base/final | No fast paths/final |
| --- | ---: | ---: | ---: | ---: | ---: |
| scroll | 767 | 337 | 352 | 2.27x | 1.04x |
| scroll-clusters | 749 | 372 | 410 | 2.01x | 1.10x |
| idle | 158 | 147 | 182 | 1.08x | 1.24x |
| repaint | 3464 | 3300 | 3424 | 1.05x | 1.04x |
| switch | 3677 | 3613 | 3661 | 1.02x | 1.01x |

The ring gives most of the scroll gain over base. A scroll no longer moves
the kept rows.

## Kept

- Fast CSI forms: short SGR codes (`1m` to `7m`, `22m`, `3Xm`, `4Xm`, `9Xm`,
  `10Xm`), the `0;` and `1;` prefixes and two colors in one sequence,
  bare `H`, `1;1H`, `A`, `B`, `C`, and `D`, and `?N h` and `?N l` for modes
  1, 7, 12, 25, 1004, 2004, and 2026. The cases sgr-short, sgr-prefix,
  csi-cursor, and csi-modes measure them.
- The AVX-512 check of 16 history lines on eviction (manyline and ascii at
  4 KiB).
- One pass over the screen rows for the snapshot row keys (snapshot idle and
  scroll).

## Removed

These paths gave less than 3%, or a loss:

- The two-parameter and six- and four-parameter SGR shortcuts in
  `escape.c`. Only the base parser reaches them, for a split sequence or in
  insert mode. A byte-at-a-time feed of sgr-pair and sgr-prefix measured
  1.00x.
- The three- and four-code-point compare in `gstore.c` (clusters, 1.02x).
- The AVX2 cell fill tail and the AVX2 ASCII put for 8 to 15 bytes in
  `kernels.h` (erase at 100 columns, 1.02x; ls-color, 0.98x; both with
  `WSAP_VTCORE_ISA=avx2`).
- `sc_ascii_fast` before `sc_print_ascii` in `text.c` (utf8, 1.00x;
  longline at 4 KiB, 0.97x).
- CRLF gaps in the history line stream (ascii, 1.00x). `stream_ascii_lines`
  already takes CRLF lines.
- The snapshot conversion without a cluster lookup (repaint, 1.01x).
- The direct snapshot path for a full update (switch, 1.00x).

## Notes

- The first version of the fault fix for the prefixes parsed every color
  through the slower two-delimiter parser. fg-rand and fgbg-char lost 11%
  to 14%. The single-color form now uses the base parser again, and the
  other forms have their own branch.
- The snapshot row conversion stores each cell as five partial writes. Its
  speed depends on code placement: an unrelated change moved the loop and
  full repaints lost 15%. Inlined, the row key pass did that, so
  `wsap_vtcore_screen_view_row_keys` stays out of line. A conversion with
  one 16-byte store for each cell would remove this sensitivity.
