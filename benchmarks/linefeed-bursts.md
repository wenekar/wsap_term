# Consecutive line feeds

The `term-write-bench` cases `lf-burst`, `crlf-burst`, `margin-lf`, and
`margin-crlf` write 64 KiB buffers in 4096-byte chunks. The margin cases use
rows 5 through 36 of a 40-row screen. All cases start at the bottom margin.

The table shows median input throughput in MiB/s. The full-screen cases used
256 iterations and 5 samples. The margin cases used 32 iterations and 3
samples. The machine used the repository's release build.

| Case | Before | After | Ratio |
| --- | ---: | ---: | ---: |
| LF, full screen | 365.93 | 1691.13 | 4.6x |
| CRLF, full screen | 624.19 | 2326.48 | 3.7x |
| LF, partial region | 9.57 | 2351.06 | 246x |
| CRLF, partial region | 19.23 | 4581.90 | 238x |

The partial-region result measures long runs of blank line feeds. The batch
path scans the run once and scrolls the region at most twice. Text between
line feeds ends the run. The full-screen path advances the row ring once and
marks only the recycled slots blank.

The CRLF margin result varied between samples. The table reports the median
from one run. Run the benchmark again on the target machine before using the
ratio as a throughput target.
