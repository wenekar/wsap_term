# Terminal line streams

This pass made CRLF output use the line stream. A tty with `ONLCR` sends
CRLF, so `cat` through a PTY takes this path. The pass also made 4 KiB LF
writes stay on the stream when a wrapped line crosses a write boundary.

## Measurement

The CPU is an AMD Ryzen 7 9700X. The compiler is GCC 16.2.1. Both builds
use the standalone core in release mode with LTO and `-Darch_flag=-march=native`.
The baseline is commit `ceeebc1`. The terminal has 120 columns, 40 rows, and
4096 history lines. The input seed is 42.

Each round runs the baseline and the current build one after the other on
CPU 4, case by case. Three rounds ran. The table gives the median of the three
sample medians. Each sample feeds a 64 KiB buffer 256 times.

```sh
taskset -c 4 term-write-bench CASE 42 0 256 5
```

Two builds of the same source in different build directories differ by up
to 3% on the CSI cases. Treat a difference below 3% as noise.

| Case | Write size | Baseline MiB/s | Current MiB/s | Ratio |
| --- | ---: | ---: | ---: | ---: |
| ascii (CRLF lines) | 65493 | 7127 | 23040 | 3.23x |
| ascii (CRLF lines) | 4096 | 3936 | 8906 | 2.26x |
| manyline | 65536 | 22330 | 22147 | 0.99x |
| manyline | 4096 | 7236 | 8029 | 1.11x |
| utf8var | 65527 | 175 | 185 | 1.06x |
| utf8var | 4096 | 173 | 181 | 1.05x |
| lf-burst | 4096 | 42076 | 43874 | 1.04x |
| crlf-burst | 4096 | 47202 | 49149 | 1.04x |
| longline | 65536 | 44489 | 44319 | 1.00x |
| longline | 4096 | 17728 | 17591 | 0.99x |
| utf8 | 4096 | 725 | 725 | 1.00x |
| cjk | 4096 | 1596 | 1592 | 1.00x |
| fgbg-char | 4096 | 2426 | 2402 | 0.99x |
| cursor | 4096 | 716 | 701 | 0.98x |
| erase | 4096 | 415 | 406 | 0.98x |
| editor | 4096 | 5588 | 5603 | 1.00x |

The other cases changed by less than 3%. `term-resize-bench` and
`hist-bench` did not change.

## Changes

- `stream_ascii_lines` replaces `stream_ascii_lf`. It takes the terminator
  width: 1 for LF in newline mode, 2 for CRLF. In other modes, "text\r\n"
  gives the same screen and history as "text\n" in newline mode.
- `scan_ascii_crlf` finds CRLF line ends. The AVX-512, AVX2, and NEON loops
  compare the CR mask, moved up one byte, with the LF mask. A carry moves a
  CR in the last byte of a block to the next block. A lone CR or LF stops the
  scan.
- The history LF push and its row kernels take the terminator width.
  `flush_orig_rows` uses it to batch the screen rows of a CRLF stream.
- When a write starts inside a wrapped line, the general path finishes only
  that line. The stream then takes the lines after it. Before, the general
  path took the rest of the write. This affected one 4 KiB write in eight.
- The CRLF flag is a bit in the existing flag group. A new `bool` after
  `ascii_tail` moved the later fields and cost 16% on `utf8` and `cjk`.

## Limits

- NEON has a compile check only.
- With 4 KiB writes, history eviction and the stream setup still cost about
  60 ns each per write. Eviction handles unaligned lines one at a time.
- Short CRLF lines before a long wrapped run still use the general path.
