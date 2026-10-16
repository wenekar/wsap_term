# Terminal write throughput

This pass is complete.
The initial target was more than 30 GiB/s for both cases.
The accepted result is about 22 GiB/s for `manyline` with 64 KiB writes.
The default benchmark also tests 4 KiB writes.
Only `longline` with 64 KiB writes exceeds 30 GiB/s.

## Measurement

The CPU is an AMD Ryzen 7 9700X.
The compiler is GCC 16.2.1.
Both builds use release mode, native instructions, LTO, and hardening.
Both builds use the same benchmark driver.
The baseline source is commit `4f7d81b`.
The terminal has 120 columns, 40 rows, and 4096 history lines.
The input seed is 42.
Each case uses 16 warmup buffers and seven timed samples.
Each timed sample feeds 16384 buffers.
Each buffer has 65536 bytes.
Each run uses CPU 4.

```sh
TERM_COLS=120 taskset -c 4 build/subprojects/term/benchmarks/term-write-bench longline 42 0 16384 7
TERM_COLS=120 taskset -c 4 build/subprojects/term/benchmarks/term-write-bench manyline 42 0 16384 7
```

The final measurements ran with no test builds active.
The table gives sample medians.
Divide the driver's MiB/s output by 1024 to get GiB/s.

| Case | Write size | Baseline GiB/s | Current GiB/s |
| --- | ---: | ---: | ---: |
| longline | 65536 | 32.32 | 44.75 |
| longline | 4096 | 5.93 | 17.94 |
| manyline | 65536 | 6.89 | 22.43 |
| manyline | 4096 | 3.60 | 7.39 |

## Changes

The LF scanner checks blocks with SIMD.
The AVX-512 path packs newline positions into 16-bit offsets.
The AVX2 and NEON paths use masks to find newline positions.
History writes use SIMD to build line descriptors.
Contiguous screen rows also use SIMD to build history descriptors.
The AVX-512 path handles 16 rows per block.
The AVX2 and NEON paths handle four rows per block.
History eviction uses cached totals for groups of 16 closed lines.
The cache checks the sequence, epoch, and terminal width.
Wrapped ASCII rows use contiguous screen storage.
The wrap path sends complete rows to history in one batch.
It copies only the rows that stay on the screen.
Adjacent row buffers use one text copy.
The batch includes the pending wrap scroll.
The row ID counter stays in a register during each stream batch.
The stream functions remain outside the large fallback function.
The dispatch calls the stream functions before the fallback allocates its queue.
The native assembly keeps the fallback stack frame out of stream calls.
Old screen rows use a 64-entry flush queue.
Tall screens use more than one flush batch.
The history path joins contiguous wrapped rows before it copies them.
LF rows that stay on the screen share one copied text buffer.
The buffer includes blank lines and LF bytes.
The flush sends complete lines from this buffer directly to history.
The retained row path sets the final text state in one step.
Stream rows use a compact ASCII form until a read or edit needs text fields.
The stream history flush uses the compact form directly.
SIMD stores set the default pen fields together.
Row counting skips division when the line fits in one row.
History skips the page boundary search when the full batch fits.
An edit copies shared text to the row's own buffer.
The stream path counts screen rows when it removes old screen content.

## Checks

The native terminal tests pass.
The flood test compares a 120-column, 80-row screen with one-byte writes.
The native model test passes.
All 50000 native write fuzz seeds pass.
The AVX2 and scalar builds pass those tests.
The ASan and UBSan terminal, history, specification, and write fuzz tests pass.
The NEON history and core sources compile with Clang for aarch64.
The compile check uses temporary C library declarations.
NEON runtime tests have not run on this host.
The write and render benchmarks complete.
The render benchmark includes all four flood cases.

## Limits

The table measures input throughput on one host.
Small writes have more screen and history work per input byte.
Both 4 KiB cases remain below 30 GiB/s.
NEON has a compile check only.
The live profile used `cat /dev/random` under headless Weston with Pixman.
It ran during this pass, before the final stream changes.
It does not measure the final source.
