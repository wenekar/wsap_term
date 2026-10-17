# Benchmark tool cost

The reference version is `7ec0f9c`.
The new tools keep the workload categories and UI frame limits.
They use less setup and repeat less work.

| Tool | Reference CPU time | New CPU time |
| --- | ---: | ---: |
| UI microbenchmarks | 34.9581 s | 1.3990 s |
| Live producer, `normal`, all cases | 6.8357 s | 0.2558 s |

These values come from one serial run on Linux with GCC 14.2.
CPU time is child user time plus child system time.
Output went to `/dev/null`. No terminal or compositor ran.
The reference producer used `g++ -O3 -march=native`.
The new tools used a Meson release build with LTO disabled and the default ISA.
These values measure tool cost. The changed workloads need new terminal baselines.

- The UI layout case runs 8,000 total passes instead of 300,000.
  It keeps seven timed samples and one warmup batch.
- Text setup fills 64 KiB instead of 64 MiB.
  A local generator replaces repeated `rand()` calls.
- Color setup prepares 256 changing frames once.
  The timed loop only writes those frames.
- The transport reader counts expected bytes.
  It takes two timestamps per transfer and does no marker search.
- The producer's static buffer is 32 MiB instead of 64 MiB.

The C and C++ benchmark sources and headers shrink from 2,659 to 1,753 lines.
The first cleanup pass had already removed 105 of those 906 lines.
