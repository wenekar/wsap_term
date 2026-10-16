# CSI Parser Benchmark

Recorded: 2026-09-10 16:35:08 +03:00 (Europe/Istanbul)  
Commit: `59f0b0c`

This benchmark evaluates escape sequence processing across complete and fragmented
inputs. Both complete commands and sequences split across arbitrary chunk boundaries
are tested for specification conformance, recovery behavior, and throughput.

## Verification

Sanitizer testing runs with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
meson test -C build term-test
```

Behavioral validation covers interrupted commands, control and high bytes,
parameter sequences, parameter limits, unsupported command sequences, and large
coordinates with origin margins. A byte-mutation test exercises all 256 byte values
across multiple positions, validating visible cells, attributes, cursor positioning,
and response bytes across arbitrary split points.

## Performance

Measurements were taken using `-O3 -march=native`. The table reports median MiB/s
from three alternating runs of `term-write-bench`.

| Workload | Baseline | Tested |
| --- | ---: | ---: |
| ASCII | 2559.50 | 2607.27 |
| UTF-8 | 1162.45 | 1180.39 |
| CJK | 2582.66 | 2652.39 |
| Colors | 652.74 | 584.96 |
| Cursor | 593.42 | 538.85 |
| Erase | 186.94 | 183.02 |
| Random | 136.01 | 137.22 |
