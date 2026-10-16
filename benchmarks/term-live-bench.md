# `cat /dev/random` Live-Terminal Profile

Run the benchmark with:

```sh
meson compile -C build wsap_term
./benchmarks/term-live-bench.sh
```

The benchmark launches the terminal with an isolated shell configuration that runs
`cat /dev/random`, sampling the terminal process at 1 ms intervals for 2 seconds.

## macOS output

macOS uses `sample(1)` and writes the raw stack profile to
`benchmarks/term-live-bench.sample.txt`.

## Linux output

Linux uses `perf record` with DWARF call graphs and writes:

- `benchmarks/term-live-bench.perf.data`: capture for interactive use with `perf report` and `perf annotate`.
- `benchmarks/term-live-bench.perf.txt`: symbolized output from `perf script`.
- `benchmarks/term-live-bench.perf.annotated.txt`: annotated hot path report.

## Throughput Comparison

Local measurements below are medians of five runs with `-O3 -march=native`, alternating
baseline and updated executables using the same benchmark harness:

| Input | Baseline MiB/s | Updated MiB/s | Change |
| --- | ---: | ---: | ---: |
| ASCII | 2765.43 | 2798.12 | +1.2% |
| UTF-8 | 1221.24 | 1244.06 | +1.9% |
| CJK | 2606.11 | 2655.00 | +1.9% |
| Colors | 683.82 | 706.62 | +3.3% |
| Cursor | 605.59 | 588.46 | -2.8% |
| Erase | 186.03 | 187.83 | +1.0% |
| Random | 120.50 | 143.41 | +19.0% |

Random input throughput improved 18.6–20.1% across seeds 1, 42, and 777 with
256-, 4096-, and 65536-byte writes:

```sh
./build/subprojects/term/benchmarks/term-write-bench random 42 4096
```
