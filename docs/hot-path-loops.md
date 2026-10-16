# Hot Path Loop Optimization Plans

This document describes five hot path loops in the terminal project.
It details memory access patterns and memory read counts for each loop.
It also describes direct indexed or hashed alternatives to replace them.

Write documentation and comments in ASD-STE100 style:
- Use short sentences.
- Use simple words.
- Use active voice.
- State one fact or instruction per sentence.

---

## 1. OSC 8 Hyperlink De-duplication

### Location
- File: `subprojects/term/src/osc/link.c`
- Function: `links_find()`

### Original Implementation
```c
static uint16_t links_find(const struct term_links *l, const char *uri)
{
    for (int i = 0; i + 1 < l->next; i++)
        if (l->uri[i] && !strcmp(l->uri[i], uri))
            return (uint16_t)(i + 1);
    return 0;
}
```

### Memory Layout and Read Counts
- `l->uri` is an array of 64-bit pointers (`char **`).
- Each pointer stores the address of a heap string allocated with `strdup()`.
- One 64-byte cache line holds 8 pointers.
- For each item in the loop:
  1. The loop reads the pointer from `l->uri[i]` (8 bytes).
  2. The loop dereferences the pointer to read the string header `l->uri[i][0]`.
  3. `strcmp()` reads subsequent bytes from the string.
- Strings sit at random heap addresses.
- Each string dereference can cause an L1 and L2 cache miss.
- Searching across $N$ links executes $2N$ to $3N$ memory reads.
- Searching across $N$ links touches $N$ distinct cache lines.

### Implemented Optimization
- `struct term_links` has a flat open-addressing hash table.
- The table stores 64-bit hashes and 16-bit link IDs:
  ```c
  struct link_hash_slot {
      uint64_t hash;
      uint16_t id;
  };
  ```
- `hash_uri()` computes the 64-bit hash of the URI.
- `links_find()` probes the table linearly.
- It compares full string contents only when the 64-bit hash matches.
- This changes average search time from $O(N)$ to $O(1)$.
- Unmatched probes require only 1 contiguous memory read.

---

## 2. Snapshot Scroll-Delta Reverse Engineering

### Location
- File: `subprojects/term/src/snapshot.c`
- Function: `term_snapshot_update_inner()`

### Original Implementation
```c
for (int k = 1; k < rows; k++) {
    if (new_keys[0] == s->keys[k]) {
        int match = 0;
        for (int y = 0; y < rows - k; y++) {
            if (new_keys[y] == s->keys[y + k])
                match++;
        }
        if (match * 2 > rows - k) {
            scrolled = k;
            break;
        }
    }
}
```

### Memory Layout and Read Counts
- `new_keys` is an array of 64-bit row generation hashes (`uint64_t`).
- `s->keys` is an array of 64-bit row generation hashes from the last snapshot.
- The outer loop reads `s->keys[k]` for each row offset $k$.
- When a key matches, the inner loop compares row keys up to `rows - k`.
- Blank terminal rows share identical hash values.
- Blank rows cause the inner loop to run frequently.
- For a window with 80 rows, this loop executes up to 3,200 memory reads per frame.
- For a window with 200 rows, this loop executes up to 20,000 memory reads per frame.

### Implemented Optimization
- Distinct snapshots (like window views and tab thumbnails) update at different intervals.
- Snapshots compare their own previous `s->keys` against current `new_keys` to detect scroll movement.
- Row keys are unique across the screen because `row->uid` and history `id` increment monotonically.
- Candidate scroll offset $k$ is found in one forward scan over the top rows.
- If row 0 changes (e.g. from an app status bar update), the search probes row 1 as a fallback.
- The verification loop exits as soon as matching rows exceed 50% ($> \text{total} / 2$).
- The verification loop also exits as soon as mismatching rows exceed 50%.
- This reduces worst-case comparisons from $O(\text{rows}^2)$ to $O(\text{rows})$.
- In typical scrolling, early exit finishes after checking only $\text{rows} / 2$ entries.

---

## 3. Screen Row Text Detection

### Location
- File: `subprojects/term/src/screen.c`
- Function: `srow_has_text()`

### Original Implementation
```c
static inline bool srow_has_text(const struct srow *r, int cols)
{
    int len = r->len < cols ? r->len : cols;
    if (r->kind == ROW_TEXT) {
        for (int x = 0; x < r->text.text_len; x++)
            if (r->text.text[x] > ' ') return true;
        return false;
    }
    for (int x = 0; x < len; x++)
        if (r->cells[x].c > ' ') return true;
    return false;
}
```

### Memory Layout and Read Counts
- `r->cells` is an array of `struct cell`.
- Each `struct cell` is 16 bytes wide:
  - `uint32_t c`
  - `uint32_t fg`
  - `uint32_t bg`
  - `uint16_t flags`
  - `uint16_t pad`
- One 64-byte cache line stores 4 cells.
- The loop reads `r->cells[x].c` with a 16-byte stride.
- For a 120-column terminal, the loop executes 120 memory reads across 30 cache lines.
- `term_screen_erase()` runs this check on every row during erase sweeps.
- Scanning 50 rows executes 6,000 memory reads across 1,500 cache lines (96 KB of data).

### Implemented Optimization
- Clang auto-vectorization refuses loops with early exits (`return true`).
- A three-tier pipeline resolves lines fast without write penalties on input ingestion:
  1. **Head probe (Scalar, column 0):** Checks `x = 0`. Most text lines start with non-space text. This resolves $>90\%$ of lines in 1 memory read and 1-2 CPU cycles.
  2. **Tail probe (Scalar, column `len - 1`):** Checks the rightmost written cell. Indented code lines end with non-space text. This resolves indented lines in 1 additional read.
  3. **SIMD sweep (Vectorized):** Sweeps the remaining middle columns in 16-byte chunks (`vld1q_u8` / `vcgtq_u8` on ARM64 NEON, `_mm_loadu_si128` / `_mm_subs_epu8` on x86) and 4-cell chunks (`vld4q_u32`).
- This avoids adding flags or write penalties to the PTY input hot path (`put_ascii_chunk`).
- Memory layout of `struct srow` remains unchanged.

---

## 4. Interactive Keymap Lookup

### Location
- File: `src/input/keybind.c`
- Function: `input_keymap_lookup()`

### Original Implementation
```c
for (int i = 0; i < km->n; i++) {
    unsigned rank = bind_rank(&km->binds[i], ev, mods);
    if (rank > best) {
        best = rank;
        action = (enum input_shortcut)km->binds[i].action;
        if (best == 3)
            break;
    }
}
```

### Memory Layout and Read Counts
- `km->binds` is an array of `struct input_keybind` (12 bytes per struct).
- Each binding stores:
  - `unsigned mods` (4 bytes)
  - `uint32_t codepoint` (4 bytes)
  - `uint16_t key` (2 bytes)
  - `uint16_t action` (2 bytes)
- For every keystroke with modifiers:
  1. The loop reads `b->mods` (4 bytes).
  2. If modifiers match, the loop reads `b->key` (2 bytes) and `b->codepoint` (4 bytes).
  3. The loop reads `b->action` (2 bytes) when the rank improves.
- For 48 configured bindings, the loop performs 48 to 96 memory reads sequentially.
- This scan runs on every user keystroke that includes modifiers.

### Implemented Optimization
- Packed the chord into a 32-bit key: `(mods << 16) | token`.
- Physical keys use bit 15 set (`key | 0x8000`). Codepoints use lowercase ASCII (`bit 15 == 0`).
- Embed a flat 256-slot open-addressing hash table in `struct input_keymap`:
  ```c
  struct input_keymap_slot hash[INPUT_KEYMAP_HASH_CAP];
  bool hash_valid;
  ```
- Memory layout is embedded directly with zero heap allocations.
- Lookup probes rank 3 first (physical key, keypad key, and layout codepoint).
- Probe returns in 1 memory read on match.
- Unbound shell navigation keystrokes (like `Ctrl+A`, `Alt+F`) detect empty slots and abort in 1 memory read.
- This drops linear scan latency from 48–96 memory reads to 1–2 reads.

---

## 5. Grapheme Cluster Interning Chain

### Location
- File: `subprojects/term/src/gstore.c`
- Function: `term_gstore_intern()`

### Original Implementation
```c
while (idx) {
    const struct gentry *e = &g->entries[idx - 1];
    if (e->allocated && (int)e->len == n &&
        !memcmp(&g->arena[e->offset], cps, (size_t)n * sizeof(*cps)))
        return GRAPHEME_HANDLE_BASE + (idx - 1);
    idx = e->next_hash;
}
```

### Memory Layout and Read Counts
- Data splits across three separate heap allocations:
  1. `g->buckets` (`uint32_t *` bucket table).
  2. `g->entries` (`struct gentry *` array, 16 bytes per entry).
  3. `g->arena` (`uint32_t *` storage of actual codepoints).
- Probing a bucket entry executes:
  1. Read from `g->buckets[b]` (4 bytes).
  2. Read `e->allocated` and `e->len` from `g->entries[idx - 1]` (3 bytes).
  3. Read `e->offset` from `g->entries[idx - 1]` (4 bytes).
  4. Read codepoints from `g->arena[offset ... offset + n - 1]`.
  5. Read `e->next_hash` on a collision (4 bytes).
- Each bucket hop crosses three different memory regions.
- This causes pointer chasing and cache misses during layout and rendering.

### Implemented Optimization
- Kept `g->entries` as the dense handle array so `term_gstore_expand()` remains $O(1)$ direct array indexing.
- Inlined the first 2 code points (`cp0`, `cp1`) directly into `struct gentry`.
- Unioned `next_free` with `offset`, keeping `sizeof(struct gentry) == 16` bytes (4 entries per cache line).
- Replaced separate chained bucket heads (`uint32_t *buckets`) with an open-addressing slot table:
  ```c
  struct gslot {
      uint32_t hash;
      uint32_t entry_idx;
  };
  ```
- Clusters with $n \le 2$ (over 85% of real-world text) compare `cp0` and `cp1` directly in registers without touching `g->arena`.
- `term_gstore_expand()` copies `cp0` and `cp1` without calling `memcpy` or reading `g->arena`.
- `term_gstore_sweep()` clears the slot table and rebuilds it in one pass without creating tombstones.
- Pointer chasing across 3 disconnected heap buffers is completely eliminated for common clusters.
