# Test Matrix Runtime

Measured on an AMD Ryzen 7 9700X (8 cores / 16 threads), Linux, GCC 16.2.1.

| Configuration | Full matrix wall time |
| --- | ---: |
| Original, serial test run | 150.7 s |
| Default 16-job parallelism, before initialization changes | 84.6 s |
| Default parallelism plus lazy terminal initialization | 35.0 s |
| Cached test objects, native pass + ISA subset (fresh build) | 17 s |
| Re-run with no source changes | 3 s |

## Performance Breakdown

In the original serial run, compilation and linking took roughly 99 seconds.
Test execution bottlenecks were primarily driven by repeated terminal allocation
under large boundary suites (creating thousands of terminal instances).

Switching from eager full-capacity initialization to lazy row allocation reduced
the terminal test suite runtime from ~15 seconds to under 0.5 seconds, while
preserving all behavioral and specification test coverage.

## Reproducing measurements

```sh
# Full test suite execution
meson test -C build

# Execution of specific test binaries
./build/subprojects/term/tests/term-test
```

## Shared test links

Measured on the same Ryzen 7 9700X with GCC 16.2.1 and Ninja 1.13.2.
Both builds use `-O3`, `-march=native`, LTO, and 16 build jobs.
Both builds start in a new directory with no object files.
Compiler caches and LTO caches are disabled.
The disk cache can contain source files and tools.

| Configuration | Compile and link time | Executables |
| --- | ---: | ---: |
| Separate test links | 35.50 s | 90 |
| Shared test links | 13.76 s | 33 |

These times include the terminal, tests, and benchmarks.
Meson setup adds about 0.5 seconds.
The shared links reduce build time by 61%.
All 90 registered tests pass with shared links.

Tests share one runner per source library.
Each test name points to its runner through a symbolic link.
The runner uses the test name to select one entry point.
Each test runs in a new process.
Tests with arguments, stubs, or allocation overrides keep separate links.

Use this command to compare fresh builds:

```sh
timing_root=$(mktemp -d)
CC=cc CCACHE_DISABLE=1 meson setup "$timing_root/standalone" \
  -Dtest_link_groups=false -Db_lto=true -Disa=native
time env CCACHE_DISABLE=1 meson compile -C "$timing_root/standalone" -j 16
CC=cc CCACHE_DISABLE=1 meson setup "$timing_root/grouped" \
  -Dtest_link_groups=true -Db_lto=true -Disa=native
time env CCACHE_DISABLE=1 meson compile -C "$timing_root/grouped" -j 16
meson test -C "$timing_root/grouped" --print-errorlogs
```
