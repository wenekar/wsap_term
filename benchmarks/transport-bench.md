# Transport Benchmark: PTY vs Fast Pipe

Run with:

```sh
meson test -C build --benchmark transport-bench --verbose
taskset -c 4 ./build/benchmarks/transport-bench
```

The benchmark streams 256 MiB per case by default.
Pass a byte budget in MiB for a short run: `build/benchmarks/transport-bench 1`.
The text payload has one newline per 80 bytes.
The random payload uses a fixed seed.

The cases cover cooked PTY text, pipe text, pipe random data, raw PTY text,
and raw PTY random data.
Cooked PTY output uses `OPOST|ONLCR`. Its expected byte count includes the added CR bytes.
The parent keeps the writer open until all expected bytes arrive.
This prevents a PTY close from dropping the last bytes.
A read timeout or failed writer makes the benchmark fail.

Timing starts before the drain and ends after the last expected byte.
The reader only counts bytes. It does not search for markers or take a timestamp per read.
The benchmark reports bytes read per second.

The numbers below used the old payload and sentinel method.
Record new baselines for the current benchmark.

## Historical numbers

Ryzen 7 9700X, Linux, GCC, `-O3 -march=native -flto`. Medians of 3 interleaved runs,
`taskset -c 4`:

| Config | Before | After | Change |
| --- | ---: | ---: | ---: |
| A standard PTY, cooked | 361 MiB/s | 356 MiB/s | — |
| B fast pipe, text | 12566 | **13814** | +9.9% |
| C fast pipe, high-entropy | 6954 | **13628** | **+96%** |

## Live terminal (TermMark)

Medians of 3 interleaved rounds under private headless Sway (`normal` size,
software rendering), identical build configuration:

| Case | Before | After | Change |
| --- | ---: | ---: | ---: |
| ManyLine | 5.89 GiB/s | **6.17** | +4.7% |
| LongLine | 4.61 | **5.39** | +17.0% |
| FGPerChar | 2.49 | **2.55** | +2.5% |
| FGBGPerChar | 3.03 | **3.09** | +1.8% |
| **TermMarkV2 Normal** | **4.15** | **4.41** | **+6.4%** |

## Takeaways

The pipe bypass achieves ~38x higher throughput over the standard cooked PTY,
which tops out near 360 MiB/s on kernel line-discipline processing and small
buffer limits. Removing marker protocol overhead brings high-entropy throughput
to parity with raw text throughput.
