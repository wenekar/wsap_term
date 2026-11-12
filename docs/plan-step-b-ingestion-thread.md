# Plan: Step B — Off-Main-Thread Ingestion

This plan moves `wsap_vtcore_feed()` off the main UI thread.
It states the Phase 0 baseline data, five architecture decisions, the facts behind each decision, and the verification steps.

## 1. Current Data Path

| Stage | Thread | Code |
| --- | --- | --- |
| Read PTY master and fastpty pipe into the 4 MiB ring | ring worker | `pty_ring_worker`, `pump` in `src/pty/ring.c` |
| Serve fastpty sync requests in pipe/tty order | ring worker | `serve_sync` in `src/pty/ring.c` |
| Wake the consumer (1 ms / 128 KiB coalescing) | ring worker | `notify`, `signal_consumer` in `src/pty/ring.c` |
| Drain the ring in chunks up to 256 KiB for up to 16 ms | main | `on_ring_event` (`PTY_RING_MAX_CHUNK`, `PTY_DRAIN_BUDGET_NS`) |
| Parse bytes into the terminal state | main | `on_ring_data` → `session_feed` → `wsap_vtcore_feed` |
| Run terminal callbacks | main | `on_term_*` in `src/session/session.c` |
| Request a frame redraw | main | `notify_output` → `tab_output` → `window_redraw` |
| Update the terminal snapshot and draw | main | `terminal_prepare` → `wsap_vtcore_snapshot_update` in `src/terminal/draw.c` |
| Write keys and replies to the PTY | main | `session_write_queue` (POLLOUT watch on the main loop) |

Facts that constrain the design:

- `wsap_vtcore` protects its state with `t->write_lock`. `wsap_vtcore_feed` holds this lock for the whole call.
- The lock is a recursive `pthread_mutex_t` (`subprojects/term/src/compat.h`). It is not fair. A thread that unlocks and re-locks immediately usually wins against a waiting thread.
- Terminal callbacks (`reply`, `title`, `clipboard`, `notify`, `bell`, `trace`) run while `wsap_vtcore_feed` holds `t->write_lock`.
- `wsap_vtcore_focus` also calls `reply` on the main thread while holding `t->write_lock`.
- Host callbacks (`tab_title`, `tab_clipboard`, `tab_bell`, `tab_output`, `tab_exited`) call window and `yasl` APIs. `yasl` allows those calls only on the main loop thread.
- `session_write_queue` arms and removes its `POLLOUT` watch with `yasl_loop_add_fd` and `yasl_loop_del_fd`. Those functions must run on the main loop thread.
- The library dependency order is `base` → `term` → `pty` → `session`. `pty` cannot include `term` headers, but `pty` invokes opaque callbacks registered by `session`.

## 2. Problem and Phase 0 Baseline

When a tab outputs dense escape sequences or random bytes, the parser runs at ~71 MiB/s (`13.4 ns/byte`).
At that rate, draining the 4 MiB ring takes ~56 ms.
`on_ring_event` spends the full 16 ms `PTY_DRAIN_BUDGET_NS` on the main thread during every loop iteration.
While `on_ring_event` runs, the main thread cannot poll input, dispatch queued keys, parse other tabs, or submit frames.

Phase 0 measured the impact on AMD Ryzen 7 9700X (`benchmarks/ingestion-thread.md`):

- **M1 (Frame interval under flood):** During `cat /dev/urandom | base64`, frame interval `dt` is `0.50 ms` at p50 and `0.80 ms` at p99, but spikes to **`18.52 ms` max** when a drain turn hits the 16 ms budget before a frame commit.
- **M2 (Idle key-to-commit latency):** `1.764 ms` at p50, `2.496 ms` at p99 (`queued -> echo parsed` takes `0.040 ms` at p50).
- **M3 (Foreground Tab B key-to-commit latency while background Tab A floods random bytes):** Latency jumps to **`61.669 ms` at p50** and **`71.511 ms` at p99** (a **35x** degradation at p50):
  - `injection -> key callback`: `6.865 ms` p50, `17.176 ms` p99 (blocked for up to one 16 ms Tab A drain turn before the loop polls input).
  - `key -> input queued`: `17.544 ms` p50, `18.026 ms` p99 (blocked for one full 16 ms Tab A drain turn before `window_drain_input` runs).
  - `queued -> echo parsed`: `35.074 ms` p50, `35.595 ms` p99 (blocked for two 16 ms Tab A drain turns before Tab B's echo is drained and rendered).
- **M4 (Main-thread `wsap_vtcore_snapshot_update` time):** `34.0 µs` at p50, `37.0 µs` at p95, `63.0 µs` at p99, `515.2 µs` max. Because parsing and snapshot updates currently share the main thread, this baseline measures pure lock + incremental copy time without cross-thread lock contention.
- **M5 (Flood throughput):** Live `termbench-v3 normal` reaches `2.292 GB/s` (`ManyLine` `9.256 GB/s`, `LongLine` `4.025 GB/s`, `FGPerChar` `1.366 GB/s`, `FGBGPerChar` `1.530 GB/s`).
- **M6 (DA1 round-trip latency):** `28.4 µs` p50 (`36.9 µs` p95) when idle. Under a same-tab flood, DA1 latency rises to **`508.4 µs` p50** (`891.4 µs` p95, `1723.0 µs` max) — a **17.9x** increase at p50.

## 3. Decisions

### D1. Pipeline: Two Stages (Parse on the Ring Worker Thread)

Options:

- **Two stages (worker thread + main UI thread).** The existing `pty_ring_worker` thread reads from the PTY/fastpty fds, maintains `serve_sync` ordering, and invokes `r->cbs.data` (`session_feed`) on the worker thread. The main thread only receives coalesced session mailbox notifications and draws frames.
- **Three stages (ring reader thread + ingestion thread + main UI thread).** `pty_ring_worker` reads into the 4 MiB ring and wakes a second ingestion thread via `eventfd`. The ingestion thread drains the ring, calls `wsap_vtcore_feed`, and wakes the main thread via another `eventfd`.

Facts:

- A three-stage pipeline spawns **2 background threads and 3+ eventfds per tab** (20 threads and 30+ fds for 10 tabs).
- In three stages, every byte is written to the 4 MiB ring on Core A and read back on Core B. At 2–9 GB/s (`ManyLine` / `LongLine`), cross-core L1/L2 cache invalidation and `memcpy` bandwidth add pure overhead.
- In three stages, every interactive keystroke echo pays two thread wakeups (`reader → ingestion → main`) plus the 1 ms `PTY_SIGNAL_INTERVAL_NS` coalescing delay unless retuned.
- `pty_ring` already owns the worker thread and already invokes the opaque `struct pty_ring_callbacks` (`data` and `ended`). Calling `r->cbs.data` on the worker thread preserves the library dependency order (`term` → `pty` → `session`): `pty` still knows nothing about `term`.
- `serve_sync` requires draining pending pipe/master bytes before replying on `sync_fd`. When `pty_ring_worker` drains into its buffer and parses chunks between `poll()` turns, `serve_sync` drains the remaining kernel pipe buffer into the ring before parsing so fastpty ordering remains exact without a second thread.

Decision: **Two stages.**
Run `r->cbs.data` (`session_feed` → `wsap_vtcore_feed`) on the existing `pty_ring_worker` thread.
Do not spawn a second thread per session.
Use the 4 MiB ring buffer as the staging buffer for `read()` and `serve_sync` so `serve_sync` can drain the kernel fd immediately into user memory before `r->cbs.data` parses it.

Check:
- Idle `input-latency-bench` (M2) stays within `50 µs` of baseline (`1.764 ms` p50) because there is no extra thread hop.
- Live `termbench` throughput (M5) stays within 3% of baseline (`2.292 GB/s`).

### D2. Lock Contention: Contention-Driven Yield Instead of Unconditional Small Slicing

Problem:
- `t->write_lock` is an unfair `pthread_mutex_t`. If the worker thread calls `wsap_vtcore_feed` in a tight loop, it unlocks and re-locks `t->write_lock` within nanoseconds, starving the main thread when `wsap_vtcore_snapshot_update` tries to acquire `t->write_lock`.
- However, unconditionally slicing `wsap_vtcore_feed` into small 32 KiB calls destroys parser fast paths. At the end of every `wsap_vtcore_feed` call, `flush_orig_rows(t)` and `stream_queue_finish(t)` materialize deferred scrollback and visible screen rows. Calling `wsap_vtcore_feed` 32 times per MiB when the main thread is **not** waiting forces 32 redundant full-screen materializations per MiB.
- Furthermore, spinning on `sched_yield()` in userspace wastes CPU and does not guarantee mutex hand-off on Linux `futex`.

Decision:

1. **Contention counter and turnstile in `wsap_vtcore`:**
   - Add to `struct wsap_vtcore`:
     - `atomic_uint waiters`: count of high-priority (main-thread) callers waiting to acquire `t->write_lock`.
     - `pthread_mutex_t yield_lock` and `pthread_cond_t yield_cond`: condition variable used to park the worker when a waiter is present.
   - Add public API functions:
     - `bool wsap_vtcore_lock_contended(const struct wsap_vtcore *t)`: returns `atomic_load_explicit(&t->waiters, memory_order_acquire) != 0`.
     - `void wsap_vtcore_yield_to_waiters(struct wsap_vtcore *t)`: if `waiters > 0`, waits on `yield_cond` until `waiters == 0`.
   - In `wsap_vtcore_lock(t)` and `wsap_vtcore_snapshot_update(t)` (main-thread entry points):
     - Increment `t->waiters` (`memory_order_acq_rel`) before acquiring `t->write_lock`.
     - Once `t->write_lock` is released in `wsap_vtcore_unlock(t)` / at the end of `wsap_vtcore_snapshot_update(t)`, decrement `t->waiters` and signal `t->yield_cond` when `t->waiters` reaches 0.
2. **Adaptive chunking inside parser / ingestion loop:**
   - When `wsap_vtcore_lock_contended(t)` is `false` (which is true for >99% of the 16.6 ms frame interval), pass full chunks (up to `256 KiB`) to `wsap_vtcore_feed` with zero extra `flush_orig_rows` overhead.
   - Inside `wsap_vtcore_feed`'s outer loop (or when feeding slices), check `atomic_load_explicit(&t->waiters, memory_order_relaxed)` every `32 KiB` boundary (or after expensive CSI operations like `ED` / `REP`).
   - Only when `waiters > 0` does `wsap_vtcore_feed` break out early (flushing deferred rows cleanly at a complete sequence boundary) so the worker can release `t->write_lock` and call `wsap_vtcore_yield_to_waiters(t)`.
3. **`PTY_DRAIN_BUDGET_NS`:**
   - Keep `PTY_DRAIN_BUDGET_NS` only for the single-threaded fallback path (`drain_pty_fd` when `pty_ring_start` is not used). The worker thread runs without a time budget.

Check:
- Main-thread wait in `wsap_vtcore_snapshot_update` (`snap_wait`, M4) stays at **p99 ≤ 500 µs** and **max ≤ 2 ms** under `cat /dev/urandom | base64`.
- `term-write-bench` and `termbench` throughput (M5) experience zero degradation on uncontended runs because 256 KiB chunks are not split unless `waiters > 0`.

### D3. Callback Delivery and Lock Ordering: Stage Inside `feed`, Flush Outside `t->write_lock`

Rules:

- **Never block on the main thread while holding `t->write_lock`.**
- **Never invoke `write()` or `eventfd` syscalls while holding `t->write_lock`.** Terminal callbacks (`on_term_*`) run inside `wsap_vtcore_feed` while `t->write_lock` is held. Performing a `write()` syscall on the PTY master fd inside `on_term_reply` while holding `t->write_lock` would extend the `t->write_lock` hold time by kernel lock contention.
- **Strict lock hierarchy:** `t->write_lock` → `session->mailbox_lock` / `session->write_queue.lock`. No code path may call `wsap_vtcore_*` while holding `mailbox_lock` or `write_queue.lock`.

Delivery per callback:

| Callback | Action Inside `wsap_vtcore_feed` (under `t->write_lock`) | Action After `wsap_vtcore_feed` Releases `t->write_lock` | Coalescing |
| --- | --- | --- | --- |
| `reply` | Append bytes to `session_write_queue` buffer in memory (under `write_queue.lock`) | Flush `session_write_queue` via non-blocking `write()` on worker thread (D4) | None; preserve exact byte order |
| `title` | Replace `pending_title` under `mailbox_lock`; set `SESSION_EVENT_TITLE` bit | Notify session `eventfd` if bitmask transitioned from 0 | Latest wins |
| `clipboard` | Replace `pending_clipboard` under `mailbox_lock`; set `SESSION_EVENT_CLIPBOARD` bit | Notify session `eventfd` if bitmask transitioned from 0 | Latest wins |
| `notify` | Push to bounded 16-entry FIFO under `mailbox_lock`; set `SESSION_EVENT_NOTIFY` bit | Notify session `eventfd` if bitmask transitioned from 0 | Drop newest when full |
| `bell` | Set `SESSION_EVENT_BELL` bit in atomic `pending_events` | Notify session `eventfd` if bitmask transitioned from 0 | One bell per main-loop dispatch |
| `output` | Set `SESSION_EVENT_OUTPUT` bit after `wsap_vtcore_feed` (unless inhibited by Mode 2026) | Notify session `eventfd` if bitmask transitioned from 0 | One redraw request per dispatch |
| `ended` | Set `SESSION_EVENT_ENDED` bit after final byte at `end_at` is parsed | Notify session `eventfd` | Runs last on main thread |
| `trace` | Write to `stderr` on calling thread | None | None |

Main-thread mailbox dispatch:
- Each session owns one `platform_eventfd` watched by the main `yasl` loop.
- The worker signals `platform_eventfd_notify` **only** when `atomic_fetch_or(&s->pending_events, mask)` transitions from `0` to non-zero, and **only after** `wsap_vtcore_feed` has released `t->write_lock`.
- On the main thread, the `eventfd` callback drains the `eventfd`, atomically swaps `s->pending_events` to `0`, and dispatches `title`, `clipboard`, `notify`, `bell`, `output`, and finally `ended` (`host->exited`).
- If `SESSION_EVENT_ENDED` is set, the handler returns immediately after `host->exited` because `tab_exited` may free the session.

Synchronized Output (DEC Mode 2026, `?2026h` / `?2026l`):
- When a TUI application runs concurrently with the main thread, it may send `?2026l` (finishing Frame $N$), wake the main thread, and immediately send `?2026h` (starting Frame $N+1$) before the main thread runs `terminal_prepare`.
- If the main thread simply skips `wsap_vtcore_snapshot_update` whenever `modes.synchronized` is currently `true`, a fast TUI application will starve and freeze until the synchronized-update timeout fires.
- To prevent both tearing and starvation:
  - While `modes.synchronized` is `true`, the worker suppresses `SESSION_EVENT_OUTPUT` until either `?2026l` is parsed or a 100 ms timeout expires.
  - When `?2026l` is parsed (transitioning `modes.synchronized` from `true` to `false`), the worker sets `SESSION_EVENT_OUTPUT`, wakes the main thread, and if a redraw is pending, waits briefly (or updates the snapshot under `t->write_lock` before starting the next `?2026h` batch) so the main thread captures the completed Frame $N$ state cleanly.

Check:
- `session-exit-order-test` passes.
- A new test sends OSC title, OSC 52 clipboard, BEL, and DA1 in a single write, verifying that the reply arrives on the PTY master and the host receives all callbacks in order before `exited`.

### D4. Thread-Safe Write Queue with Deferred Syscall Flush

Problem:
- Terminal replies (`DA1`, `DECRQM`, `CPR`, `OSC 10/11`, kitty keyboard query) originate on the worker thread during `wsap_vtcore_feed`, while keystrokes, paste data, and focus replies originate on the main thread.
- Routing replies through the main-thread mailbox would couple DA1 latency (M6) to main-thread frame rendering.
- However, calling `yasl_loop_add_fd` / `yasl_loop_del_fd` from the worker thread is illegal, and calling `write()` while holding `t->write_lock` blocks the main thread from taking snapshots.

Decision:
1. Protect `session_write_queue` buffer state (`buf`, `len`, `cap`, `head`) with `pthread_mutex_t lock`. Keep `watching` (`yasl_loop` `POLLOUT` registration) strictly on the main thread.
2. During `wsap_vtcore_feed`, `on_term_reply` calls `session_write_queue_stage(q, data, len)`, which locks `q->lock` and appends bytes in memory **without** calling `write()`.
3. Immediately after `wsap_vtcore_feed` releases `t->write_lock`, the worker calls `session_write_queue_flush(q)`:
   - Locks `q->lock` and attempts non-blocking `write()` syscalls while `q->len > 0`.
   - If `write()` returns `EAGAIN` / `EWOULDBLOCK` and bytes remain queued, sets `SESSION_EVENT_ARM_WRITE_WATCH` in the session mailbox so the main thread arms `POLLOUT`.
4. Main-thread callers (`session_send`, `session_paste`, `wsap_vtcore_focus`) read `wsap_vtcore_modes` **before** acquiring `q->lock`, stage their bytes, and flush via `session_write_queue_flush`.
5. Each staged write is contiguous. Concurrent writes from the worker thread (replies) and main thread (keystrokes) interleave only at complete message boundaries.
6. Rate-limit the `QUEUE_LIMIT` overflow warning so a flood of terminal queries cannot spam `stderr`.

Check:
- `paste-test` passes.
- DA1 round-trip latency under flood (M6) drops from `508.4 µs` p50 to **≤ 150 µs p50** and **≤ 1 ms p99**.
- A new multi-threaded TSan test pushes replies and keystrokes concurrently and verifies that every message arrives intact and in per-thread order.

### D5. Shutdown Order

`session_fini` teardown sequence:

1. Remove the session from `live_sessions`.
2. Call `pty_ring_stop(&s->ring)`:
   - Sets `r->stop = true`, wakes `space_wr` and `sync_fd`, and wakes `wsap_vtcore` if parked in `wsap_vtcore_yield_to_waiters`.
   - Joins `pty_ring_worker`. Because the worker never blocks on the main thread (D3), `pthread_join` cannot deadlock.
3. Remove the session mailbox `eventfd` watch from `s->loop` and close the `eventfd`.
4. Free any unconsumed `pending_title`, `pending_clipboard`, and notification FIFO entries without invoking host callbacks.
5. Destroy `s->write_queue` (removing any `POLLOUT` watch on `s->loop` and destroying `q->lock`). No worker thread is running now.
6. Remove fallback fd watches (if used) and call `pty_destroy(&s->pty)`.
7. Call `wsap_vtcore_free(s->term)` and free `s->title`.

Process exit sequence (`session_reap_exited`):
- Calls `pty_ring_end(&s->ring)`.
- The worker finishes draining and parsing all bytes up to `end_at`, flushes pending replies, and sets `SESSION_EVENT_OUTPUT | SESSION_EVENT_ENDED` in the session mailbox.
- The main thread processes `SESSION_EVENT_OUTPUT` first and calls `host->exited` last.

Check:
- `session-exit-order-test` and `pty-destroy-test` pass.
- A new stress test closes a tab during an active flood 100 times under ASan and TSan.

## 4. Main-Thread Lock Reduction and State Ownership (Completed in Phase 0)

Phase 0 audited and fixed main-thread locking and probe state in preparation for concurrent ingestion:

- **Reduced per-frame `wsap_vtcore` lock acquisitions from 9 to 1:**
  - Added `int history_lines` to `struct wsap_vtcore_view` (`subprojects/term/include/wsap_vtcore/wsap_vtcore.h`) and populated it inside `wsap_vtcore_snapshot_update`.
  - Added `bool snapshot_prepared` to `struct terminal` (`src/terminal/internal.h`) so `terminal_frame_source` reuses the snapshot updated in `terminal_prepare` instead of locking and updating a second time per frame.
  - Updated `root_prepare` (`src/terminal/root.c`), `blinking` (`src/terminal/blink.c`), `update_ime` (`src/terminal/draw.c`), and `cursor_visible` (`src/terminal/draw.c`) to read cursor coordinates, cursor visibility, blink state, palette, and scrollback metrics from `wsap_vtcore_snapshot_view(tw->snapshot)` without locking `wsap_vtcore`.
- **Batched compound main-thread reads under single lock holds:**
  - Wrapped multi-call `wsap_vtcore_*` sequences in `src/terminal/draw.c` (`terminal_scroll_metrics`), `src/terminal/input.c`, `src/terminal/selection.c`, `src/terminal/view.c`, and `src/terminal/menu.c` in single `wsap_vtcore_lock` / `wsap_vtcore_unlock` critical sections.
- **Made `latency_probe` thread-safe and SIMD-accelerated:**
  - Converted `active`, `rendered`, and `stamps[]` in `src/loop/latency_probe.c` to `_Atomic uint64_t`.
  - Moved `latency_probe_scan_output` parser state to `_Thread_local` and used `memchr(data + i, '\033', len - i)` so non-ESC flood runs skip at SIMD speed without cross-tab interference.
- **Transport newline state (`onlcr`, `newline_synced`, `newline_fd`):**
  - Accessed only by `session_feed` on the worker thread (`tcgetattr` is thread-safe).

## 5. Thread Ownership After Step B

| State | Owner Thread | Synchronization |
| --- | --- | --- |
| PTY master reads, fastpty pipe reads, `serve_sync`, `onlcr`, `newline_*` | ring worker | Single-thread access on worker |
| `s->term` (`struct wsap_vtcore`) | shared (worker writes, main snapshots/queries) | `t->write_lock` + `t->waiters` turnstile (D2) |
| Session mailbox (`pending_events`, `pending_title`, `pending_clipboard`, `notify` FIFO) | shared (worker posts, main drains) | Atomic `pending_events` + `s->mailbox_lock` |
| `session_write_queue` buffer (`buf`, `len`, `head`) | shared (worker stages/flushes replies, main sends input) | `q->lock` mutex (never held while acquiring `t->write_lock`) |
| `session_write_queue` `POLLOUT` watch, `s->title`, host/UI callbacks, `tw->snapshot` | main | Single-thread access on main loop |

## 6. Implementation Phases

Each phase compiles cleanly, passes `python3 tools/dev/dirgraph.py build`, passes `meson test -C build`, and can merge independently.

0. **Baseline and main-thread lock audit (Done).**
   - Reduced per-frame main-thread locks from 9 to 1.
   - Fixed double snapshot updates, `frame_stats` histogram resolution, `latency_probe` thread safety, and `input-latency-bench --bg-flood`.
   - Recorded M1–M6 baseline numbers in `benchmarks/ingestion-thread.md`.
1. **Term contention turnstile API (D2).**
   - Add `waiters`, `yield_lock`, `yield_cond`, `wsap_vtcore_lock_contended`, and `wsap_vtcore_yield_to_waiters` to `subprojects/term/`.
   - Add adaptive 32 KiB contention check inside `wsap_vtcore_feed` when `waiters > 0`.
   - Add unit test in `subprojects/term/tests/` verifying that a contended reader acquires `t->write_lock` within bounded time while a writer floods.
2. **Thread-safe `session_write_queue` with stage/flush split (D4).**
   - Add mutex protection to `struct session_write_queue`, split `stage` (in-memory) from `flush` (non-blocking `write()` outside `t->write_lock`), and add `POLLOUT` arm callback for the main thread.
   - Add multi-threaded write-queue test.
3. **Session mailbox and callback staging (D3).**
   - Route `title`, `clipboard`, `notify`, `bell`, `output`, `ended`, and `arm_write_watch` through the per-session `platform_eventfd` and mailbox while `session_feed` still runs on the main thread.
   - Verify all existing session tests and new callback-ordering test pass.
4. **Move `session_feed` to `pty_ring_worker` (D1, D2, D5).**
   - Update `pty_ring_worker` and `session` so the worker thread drains the ring in chunks, calls `session_feed` (`wsap_vtcore_feed`), flushes staged replies and mailbox events after releasing `t->write_lock`, and yields via `wsap_vtcore_yield_to_waiters` when the main thread requests a snapshot.
   - Update `session_fini` shutdown order (D5).
5. **Final verification and benchmark update.**
   - Run full test suite under ASan and TSan.
   - Re-run M1–M6 benchmarks and record the before/after comparison in `benchmarks/ingestion-thread.md`.

## 7. Metrics and Acceptance Targets

Measured on AMD Ryzen 7 9700X (median of 5 runs):

| ID | Metric | Baseline (Phase 0) | Target (After Step B) |
| --- | --- | --- | --- |
| M1 | Frame interval `dt` under flood | p99 `0.80 ms`, max **`18.52 ms`** | p99 ≤ `2.0 ms`, max ≤ `4.0 ms` |
| M2 | Key-to-commit latency, idle | p50 **`1.764 ms`**, p99 **`2.496 ms`** | p50 ≤ `1.814 ms` (baseline + `50 µs`) |
| M3 | Key-to-commit latency in Tab B while Tab A floods random bytes | p50 **`61.669 ms`**, p99 **`71.511 ms`** | p50 ≤ `2.5 ms`, p99 ≤ `5.0 ms` |
| M4 | Main-thread wait in `wsap_vtcore_snapshot_update` under flood | p50 `34.0 µs`, p99 `63.0 µs`, max `515.2 µs` | p99 ≤ `500 µs`, max ≤ `2000 µs` |
| M5 | Flood throughput (`termbench-v3 normal`) | **`2.292 GB/s`** | ≥ `2.223 GB/s` (within 3% of baseline) |
| M6 | DA1 round-trip latency under flood | idle p50 `28.4 µs`; flood p50 **`508.4 µs`** | idle ≤ `35 µs` p50; flood ≤ `150 µs` p50, ≤ `1000 µs` p99 |

## 8. Tests

- `python3 tools/dev/dirgraph.py build` reports zero violations or cycles.
- `meson test -C build` passes all unit and integration tests.
- TSan and ASan runs pass on `pty` and `session` test suites.
- New behavioral tests:
  - Contended snapshot wait latency under continuous `wsap_vtcore_feed` flood (D2).
  - Callback ordering and DA1 reply delivery in one compound write (D3).
  - Concurrent reply and input pushes on `session_write_queue` (D4).
  - Repeated tab close during active flood under ASan/TSan (D5).
  - Concurrent `wsap_vtcore_resize` during `wsap_vtcore_feed` flood verifying snapshot grid consistency.
