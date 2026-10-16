# Termbench & Live Profiling Investigation Notes

## Summary of Findings

1. **Escape Sequence Parsing Optimization**:
   - Truecolor sequence parsing was unrolled to parse color components directly without sequential function call overhead.
   - Benchmark results (`term-write-bench`):
     - `fgbg-char`: rose from ~2880 MiB/s to **3623.45 MiB/s** (+25.7% speedup).
     - `fg-char`: rose from ~2640 MiB/s to **2934.05 MiB/s** (+11.1% speedup).
     - `longline`: **6091.40 MiB/s** (~6.1 GB/s).
     - `ascii`: **5758.61 MiB/s**.

2. **Transport and Live Redraw Profile**:
   - In live Wayland sessions, throughput jumps from standard PTY levels (~0.6–0.7 GB/s) to 1.5–1.6 GB/s with fast pipe enabled, and in headless execution to >2.5–2.8 GB/s.
   - Under continuous rapid color changes, perceptual color coverage calculations during glyph blending are the main rendering cost.

## Benchmark Script

The runner script is available at `benchmarks/run-termbench.sh`.

### How to Run
```bash
./benchmarks/run-termbench.sh [small|normal|large]
```
