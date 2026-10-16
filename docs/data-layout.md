# Data Structure Layout & Alignment Guide

This document captures core memory layout constraints, cacheline isolation requirements, SIMD invariants, and layout optimizations across hot and warm data structures in the terminal emulator, as analyzed by `pahole`.

---

## 1. Inspection with `pahole`

The release build (`debug: false`) strips DWARF symbols. To inspect layout with `pahole`:

- **Full binary inspection** (via Meson debug build):
  ```bash
  meson configure build -Ddebug=true
  ninja -C build bin/wsap_term
  pahole -C <struct_name> build/bin/wsap_term
  ```

- **Per-translation unit inspection**:
  ```bash
  gcc -Isubprojects/term/include -Isubprojects/term/src -g -c subprojects/term/src/term.c -o /tmp/term.o && pahole -C term /tmp/term.o
  gcc -Isrc -g -c src/pty/ring.c -o /tmp/ring.o && pahole -C pty_ring /tmp/ring.o
  ```

---

## 2. Core Data Structures & Invariants

### A. `struct term_cell` (`subprojects/term/include/term/term.h`) — Hot Data Plane

```c
struct term_cell {
    uint32_t c;      /* 0  4 */
    uint32_t fg;     /* 4  4 */
    uint32_t bg;     /* 8  4 */
    uint16_t flags;  /* 12 2 */
    uint16_t pad;    /* 14 2 */
    /* size: 16, cachelines: 1, members: 5 */
};
```

- **Size & Alignment**: Exactly 16 bytes, naturally aligned, 0 holes.
- **AVX-512 Invariant**: Exactly 4 cells fit into a 64-byte AVX-512 register (`__m512i`).
- **SIMD Packing**: High-throughput routines in `subprojects/term/src/text.c` treat bytes 12–15 (`flags` and `pad`) as a combined 32-bit dword (`flags | (pad << 16)`).
- **Rule**: Do not alter field order or resize `struct term_cell` without adjusting AVX-512 / AVX2 permutation routines and chunk-write kernels.
- **Colors**: `fg` and `bg` hold either a literal ARGB color (alpha `0xFF`, from SGR `38;2`) or a palette slot (top byte `0x01`, `TERM_COLOR_SLOT`). A slot names palette entry 0–255, `TERM_COLOR_FG`, or `TERM_COLOR_BG`. The renderer resolves slots with `terminal_frame_color()`, which calls `term_view_color()`, when it paints. A theme change or an OSC 4/10/11 therefore recolors text already on screen. Writers copy colors as opaque dwords and never resolve them. `terminal_frame_init()` copies the view's `palette_version` into `full_gen`, so every backend repaints fully when the palette changes.

---

### B. `struct pty_ring` (`src/pty/ring.h`) — Multi-Threaded PTY Ring Buffer

```c
struct pty_ring {
    char * buf;                  /* 0   8 */
    struct yasl_loop * loop;     /* 8   8 */
    pthread_t thread;            /* 16  8 */
    struct pty_ring_cbs cbs;     /* 24 16 */
    void * ud;                   /* 40  8 */
    int read_fd;                 /* 48  4 */
    int event_fd;                /* 52  4 */
    int event_wr;                /* 56  4 */
    int space_fd;                /* 60  4 */
    /* --- cacheline 1 boundary (64 bytes) --- */
    int space_wr;                /* 64  4 */
    int event_watch;             /* 68  4 */
    _Atomic int stop;            /* 72  4 */
    bool active;                 /* 76  1 */
    /* 51 bytes intentional cacheline padding */
    /* --- cacheline 2 boundary (128 bytes) --- */
    _Atomic size_t head __attribute__((__aligned__(64))); /* 128 8 */
    _Atomic int need_space;      /* 136 4 */
    /* 52 bytes intentional cacheline padding */
    /* --- cacheline 3 boundary (192 bytes) --- */
    _Atomic size_t tail __attribute__((__aligned__(64))); /* 192 8 */
    _Atomic int awake;           /* 200 4 */
    /* size: 256, cachelines: 4 */
};
```

- **Cacheline Isolation (Anti-False Sharing)**:
  `pahole` reports 103 bytes of "holes" around `head` and `tail`. **These holes are deliberate 64-byte cacheline isolation boundaries.**
  - `head` is mutated by the PTY reader worker thread (`pty_ring_worker`).
  - `tail` is mutated by the main Wayland loop thread (`on_ring_event`).
  - Merging or packing `head` and `tail` into shared cachelines causes severe cache bounce and stalls throughput. **Never pack or remove these padding holes.**

---

### D. `struct term` (`subprojects/term/src/internal.h`) — Terminal Emulator State

- **Screen storage**: `scr` and `alt_scr` each own a ring of `rows` slots,
  a cell slab, and a text slab. `spare` holds the buffers that the next
  resize builds into; `resize` holds grow-only scratch buffers. A resize to
  sizes the terminal has seen allocates nothing.
- **Row metadata**: `struct srow` holds the cells pointer, a `text_row`, the
  row ID, the version, the length, the kind (`ROW_CELLS` or `ROW_TEXT`), and
  the flags. A row has no line ID: the line of each row follows from the
  history tail and the `RF_WRAPPED` chain above it.
- **Text rows**: a `ROW_TEXT` row stores its content in the history format:
  text bytes, and attribute runs in a `hist_runs` with a fixed buffer. Its
  run count comes before the pens, so a wide store of the pens never covers
  the count, which the next append reads back at once.
- **History storage**: `struct hist` stores encoded logical lines in a ring
  of 16-byte parts in 64 KiB pages. Line IDs count logical lines; a line of
  several parts has one ID. History does not use the screen cell slab or
  grapheme handles.
- **Inner loop working set**: The write core uses the cursor, screen dimensions,
  screen ring index, and active row. A scroll copies a text row into history
  and encodes a cells row.
- **Resize**: Normal-screen resize reflows logical lines. Alternate-screen resize
  preserves the saved normal screen until it becomes active.

---

## 3. Verification Checklist

Whenever modifying data structures:

1. **Zero Initialization**: Ensure all types obey `Zero Is Initialization` (`AGENTS.md`).
2. **Layout Verification**:
   ```bash
   gcc -Isrc -g -c <source_file>.c -o /tmp/test.o && pahole -C <struct_name> /tmp/test.o && rm -f /tmp/test.o
   ```
3. **Headless Throughput**:
   ```bash
   ./build/subprojects/term/benchmarks/term-write-bench manyline && ./build/subprojects/term/benchmarks/term-write-bench longline
   ```
4. **Unit Tests**:
   ```bash
   meson test -C build
   ```
