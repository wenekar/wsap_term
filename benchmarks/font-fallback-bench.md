# Font Fallback Benchmark: Coverage Cache

Run with:

```sh
meson test -C build --benchmark font-fallback-bench --verbose
```

The benchmark measures the FreeType fallback coverage cache.
The CoreText build does not include it.

After font initialization, it does these steps:

- **Scan:** `ft_scan_run` checks each codepoint once.
  The codepoints are U+0020–U+FFFF without surrogates, and U+1F000–U+1FAFF.
  Pass 1 fills the cache. Pass 2 reads it.
  The render path calls this function for every shaped run.
- **Runs:** `run_coverable` checks two-codepoint runs.
  It pairs each BMP codepoint with `a`.
  It pairs each codepoint in U+2E80–U+9FFF with U+4E00.
  A run-cache miss does this fallback search.
- **Heap growth:** the glibc heap growth during the scan and run steps.
  Other C libraries do not print this line.

The `results=` hashes record the answers.
The answers depend on the installed fonts.
Compare the hashes only on one machine.
A cache change must not change them.

## Numbers (Linux, x86-64, GCC 16.2, `-O3 -march=native -flto`)

Measured on a Ryzen 7 9700X, pinned with `taskset -c 2`.
The machine has 1,170 font faces in `fc-list`.
Each column shows three runs.

| Step | Four caches (7ec0f9c) | Paged table |
| --- | ---: | ---: |
| Scan pass 1 | 40.0–40.3 ms | 10.9–11.0 ms |
| Scan pass 2 | 0.44 ms | 0.15 ms |
| Runs | 2.13–2.20 ms | 7.2–8.6 ms |
| Heap growth | 13,514 KiB | 520 KiB |

## Interpretation

The old code kept four caches.
Each cache had a shared table for the BMP and a per-thread hash for other codepoints.
The four BMP tables take 2 MiB.
The old code also stored a list of covering fallback fonts for each codepoint.
With 1,170 fonts, each list takes 152 bytes.
These lists take most of the remaining heap.

The new code keeps one table for all of Unicode.
Each page holds 256 codepoints.
The code allocates a page on first use.
Each entry is 64 bits: the configured faces that cover the codepoint, a coverable bit, and a known bit.
The full BMP needs 256 pages, which is 512 KiB.

The run step is about 3x slower.
The new code stores no font lists.
Each run tests candidate fonts in order until one font covers the run.
The page index removes most candidates first.
This search runs only on a run-cache miss, and HarfBuzz shaping follows it.

The run hashes match. The scan hashes differ.
The old code reports 3,606 uncovered codepoints. The new code reports 3,738.
The old per-thread table for codepoints above the BMP held 2,048 entries.
When it was full, it reported each new codepoint as covered by all configured faces.
The scan fills that table at U+1F800.
The 132 extra codepoints all come after U+1F800, and most of them are unassigned.
