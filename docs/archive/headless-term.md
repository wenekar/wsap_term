# Headless terminal core migration

Historical plan. See [the core guide](../../subprojects/term/README.md) for the current API.

## 1. Objective and Scope

Make `subprojects/term/` completely headless, self-contained, and isolated from repository internals.

### Core requirements
- Zero dependencies outside the `subprojects/term/` directory tree.
- Standard C11, POSIX system headers, and compiler SIMD intrinsic headers (`<immintrin.h>`, `<arm_neon.h>`).
- C11 syntax compliance: include `<stdalign.h>` for `alignas` in `text_run.c`.
- System link dependencies: C runtime, `libm` (`-lm` for color gamma math), and `libpthread` (`-lpthread` for thread safety checks).
- Define canonical geometry types (`struct term_vector2i`, `struct term_rect2i`) within `subprojects/term/`. Provide explicit conversion helpers at application boundaries as an approved exception to `AGENTS.md`.
- Preserve the existing snapshot contract, palette resolution model, rich input encoding, selection units, scrollback navigation, writer synchronization, and accessibility history export.
- Implement Unicode utilities (`utf8_encode`, `unicode_width`) strictly as `static inline` functions within `subprojects/term/include/term/unicode/` to guarantee zero static-archive link collisions.
- Build as an isolated static library (`libterm_headless.a`) via a standalone `meson.build` with explicit ISA options, and integrate cleanly into root `src/meson.build`.

---

## 2. Dependency Audit and Decoupling Plan

Every external header currently in `subprojects/term/` is audited below with its exact call sites and decoupling plan:

| Header | Files in `subprojects/term/` | Current usage | Decoupling strategy |
| :--- | :--- | :--- | :--- |
| `util/geom.h` | `term.h`, `screen.h`, `selection.c` | `struct vector2i`, `struct rect2i` | Define canonical `struct term_vector2i { int x, y; }` and `struct term_rect2i { struct term_vector2i pos, size; }` in `term_types.h`. Application wrappers convert explicitly at boundaries. |
| `paint/blend.h` | `palette.c:8` | sRGB/linear transfer functions (`powf`) | Internalize gamma transfer functions directly into `palette.c`. Link with `-lm`. |
| `util/hash.h` | `gstore.c:5`, `osc/link.c:5` | Hash table slot mixing (`hash64_step`, `hash64_finish`) | Internalize 64-bit integer mixing functions into `internal.h`. (These are hash table slot lookups, not row keys). |
| `platform/platform.h` | `screen.c:4` | Unused platform header | Remove include. Headless core uses standard POSIX interfaces. |
| `input/input.h` | `input.c`, `term.c` | Keycodes, actions, modifiers, and keyboard state | Define standalone `enum term_key`, `enum term_key_action`, `enum term_mod`, and `struct term_keyboard_state` in `term_input.h`. |
| `unicode/utf8.h` | `core.c`, `text.c`, `hist.c`, `input.c`, `selection.c`, `text_run.c` | UTF-8 decode, encode, and validation | Provide `utf8_encode()` and decode helpers as `static inline` functions in `subprojects/term/include/term/unicode/utf8.h`. (Using `static inline` guarantees zero duplicate symbol collisions in static archives). |
| `unicode/unicode.h` | `core.c`, `text.c`, `screen.c`, `text_run.h` | Character width tables and emoji properties | Provide `unicode_width()` and category queries strictly as `static inline` functions in `subprojects/term/include/term/unicode/width.h` backed by local table data. |
| `unicode/grapheme.h` | `core.c`, `gstore.c`, `hist.c`, `selection.c`, `snapshot.c`, `text.c`, `text_run.h` | Grapheme boundary queries and cluster helpers | Internalize grapheme break logic. Ensure cluster tables remain per-terminal instance in `gstore.c` to prevent global mutable state or duplicate link symbols. |

---

## 3. Public API Specification (`subprojects/term/include/term/term.h`)

### 3.1. Types, Geometry, and Colors

```c
#ifndef HEADLESS_TERM_H
#define HEADLESS_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct term;
struct term_snapshot;

struct term_vector2i {
    int x;
    int y;
};

struct term_rect2i {
    struct term_vector2i pos;
    struct term_vector2i size;
};

struct term_span {
    int begin;
    int end;
};

typedef uint32_t term_color;

#define TERM_COLOR_SLOT  0x01000000u
#define TERM_COLOR_FG    (TERM_COLOR_SLOT | 0x100u)
#define TERM_COLOR_BG    (TERM_COLOR_SLOT | 0x101u)

static inline term_color term_color_index(int n)
{
    return TERM_COLOR_SLOT | ((uint32_t)n & 0xFFu);
}

static inline term_color term_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline bool term_color_is_slot(term_color c)
{
    return (c >> 24) == (TERM_COLOR_SLOT >> 24);
}

enum {
    TERM_PALETTE_FG     = 256,
    TERM_PALETTE_BG     = 257,
    TERM_PALETTE_CURSOR = 258,
    TERM_PALETTE_SIZE   = 259,
};

enum term_attr {
    TERM_ATTR_NONE             = 0,
    TERM_ATTR_BOLD             = 1u << 0,
    TERM_ATTR_ITALIC           = 1u << 1,
    TERM_ATTR_FAINT            = 1u << 2,
    TERM_ATTR_STRIKE           = 1u << 3,
    TERM_ATTR_OVERLINE         = 1u << 4,
    TERM_ATTR_LINK             = 1u << 5,
    TERM_ATTR_EMOJI            = 1u << 6,
    TERM_ATTR_TEXT             = 1u << 7,
};

#define TERM_ATTR_UNDERLINE_SHIFT 8
#define TERM_ATTR_UNDERLINE_MASK  (7u << TERM_ATTR_UNDERLINE_SHIFT)

enum term_underline {
    TERM_UNDERLINE_NONE = 0,
    TERM_UNDERLINE_SINGLE,
    TERM_UNDERLINE_DOUBLE,
    TERM_UNDERLINE_CURLY,
    TERM_UNDERLINE_DOTTED,
    TERM_UNDERLINE_DASHED,
};

static inline enum term_underline term_attr_underline(uint16_t attrs)
{
    return (enum term_underline)((attrs & TERM_ATTR_UNDERLINE_MASK) >> TERM_ATTR_UNDERLINE_SHIFT);
}

#define TERM_TEXT_CLUSTER 0x80000000u

struct term_cell {
    uint32_t text;
    term_color fg;
    term_color bg;
    uint16_t attrs;
    uint8_t  width;
    uint8_t  reserved;
};

_Static_assert(sizeof(struct term_cell) == 16, "struct term_cell must remain 16 bytes");
```

### 3.2. Lifecycle, Synchronization, Configuration, and Callbacks

```c
enum term_cursor_style {
    TERM_CURSOR_BLOCK = 0,
    TERM_CURSOR_UNDERLINE = 1,
    TERM_CURSOR_BAR = 2,
};

struct term_theme {
    uint32_t fg, bg, cursor;
    uint32_t ansi[16];
};

void term_theme_default(struct term_theme *theme);
void term_theme_high_contrast(struct term_theme *theme);
bool term_theme_set_key(struct term_theme *p, const char *key, const char *val);

struct term_callbacks {
    void *ud;
    void (*reply)(void *ud, const char *bytes, size_t len);
    void (*title)(void *ud, const char *title);
    void (*clipboard)(void *ud, const char *selection, const char *data, size_t len);
    void (*notify)(void *ud, const char *title, const char *body);
    void (*bell)(void *ud);
};

struct term_options {
    struct term_vector2i size;
    int scrollback;
    struct term_theme theme;
    enum term_cursor_style cursor_style;
    bool cursor_steady;
    struct term_callbacks callbacks;
};

struct term *term_new(const struct term_options *opts);
void         term_free(struct term *t);

void         term_lock(struct term *t);
void         term_unlock(struct term *t);

bool         term_resize(struct term *t, struct term_vector2i size);

enum term_feed_flags {
    TERM_FEED_ONLCR = 1u << 0,
};

void         term_feed(struct term *t, const char *bytes, size_t len, unsigned flags);
void         term_focus(struct term *t, bool focused);

void         term_set_theme(struct term *t, const struct term_theme *theme);
void         term_set_cursor_style(struct term *t, enum term_cursor_style style);
void         term_set_cursor_blink(struct term *t, bool blink);
void         term_set_cursor_default(struct term *t, enum term_cursor_style style, bool steady);
```

### 3.3. Snapshot Viewport, Selection, and History Inspection

```c
struct term_cursor {
    struct term_vector2i pos;
    enum term_cursor_style style;
    bool visible;
    bool blink;
};

struct term_view {
    struct term_vector2i size;
    struct term_cursor   cursor;
    int                  scroll_offset;
    int                  scrolled;
    bool                 reverse_video;
    uint64_t             version;
    uint64_t             palette_version;
    const uint32_t      *palette;
};

struct term_snapshot *term_snapshot_new(void);
void                  term_snapshot_free(struct term_snapshot *s);
bool                  term_snapshot_update(struct term_snapshot *s, struct term *t);

const struct term_view *term_snapshot_view(const struct term_snapshot *s);
const struct term_cell *term_snapshot_row(const struct term_snapshot *s, int y);
uint64_t                term_snapshot_row_key(const struct term_snapshot *s, int y);
struct term_span        term_snapshot_row_selection(const struct term_snapshot *s, int y);

int term_snapshot_cluster(const struct term_snapshot *s, uint32_t text,
                          uint32_t *cps, int cap);

static inline uint32_t term_view_color(const struct term_view *v, term_color c)
{
    if (!term_color_is_slot(c))
        return c;
    return v->palette[c & 0x1FFu];
}

struct term_vector2i term_size(const struct term *t);
struct term_cursor   term_cursor(const struct term *t);
const char          *term_cwd(const struct term *t);
const char          *term_pointer_shape(const struct term *t);
int                  term_history_rows(const struct term *t);
char                *term_history_text(struct term *t, int max_rows);
char                *term_link_at(const struct term *t, struct term_vector2i cell);

struct term_scrollback {
    int offset;
    int total;
};

struct term_scrollback term_scrollback(const struct term *t);
void                   term_scroll(struct term *t, int lines);
void                   term_scroll_to(struct term *t, int offset);
void                   term_scroll_to_top(struct term *t);
void                   term_scroll_to_bottom(struct term *t);

enum term_select_unit {
    TERM_SELECT_CELL,
    TERM_SELECT_WORD,
    TERM_SELECT_LINE,
};

void term_select_start(struct term *t, struct term_vector2i cell, bool right_half,
                       enum term_select_unit unit);
void term_select_extend(struct term *t, struct term_vector2i cell, bool right_half);
void term_select_clear(struct term *t);
bool term_has_selection(const struct term *t);
char *term_selection_text(const struct term *t);
```

### 3.4. Input and Mouse Encoding Contract

```c
enum term_key_action {
    TERM_KEY_PRESS = 1,
    TERM_KEY_REPEAT = 2,
    TERM_KEY_RELEASE = 3,
};

enum term_mod {
    TERM_MOD_SHIFT = 1u << 0,
    TERM_MOD_ALT   = 1u << 1,
    TERM_MOD_CTRL  = 1u << 2,
    TERM_MOD_SUPER = 1u << 3,
    TERM_MOD_HYPER = 1u << 4,
    TERM_MOD_META  = 1u << 5,
    TERM_MOD_CAPS  = 1u << 6,
    TERM_MOD_NUM   = 1u << 7,
};

struct term_keyboard_state {
    uint64_t down[4];
};

struct term_key_event {
    enum term_key_action action;
    uint16_t key;
    uint16_t keypad;
    unsigned mods;
    uint32_t codepoint;
    uint32_t shifted;
    uint32_t base;
    uint32_t text;
};

enum term_mouse_event_kind {
    TERM_MOUSE_PRESS,
    TERM_MOUSE_RELEASE,
    TERM_MOUSE_MOTION,
    TERM_MOUSE_SCROLL,
};

struct term_mouse_event {
    enum term_mouse_event_kind kind;
    int button;
    struct term_vector2i pos;
    struct term_vector2i pixel;
    unsigned mods;
    int scroll_lines;
};

enum term_mouse {
    TERM_MOUSE_OFF    = 0,
    TERM_MOUSE_X10    = 9,
    TERM_MOUSE_NORMAL = 1000,
    TERM_MOUSE_BUTTON = 1002,
    TERM_MOUSE_ANY    = 1003,
};

enum term_mouse_encoding {
    TERM_MOUSE_ENC_X10        = 0,
    TERM_MOUSE_ENC_UTF8       = 1005,
    TERM_MOUSE_ENC_SGR        = 1006,
    TERM_MOUSE_ENC_URXVT      = 1015,
    TERM_MOUSE_ENC_SGR_PIXELS = 1016,
};

struct term_modes {
    unsigned keyboard;
    enum term_mouse mouse;
    enum term_mouse_encoding mouse_encoding;
    struct term_vector2i grid;
    bool app_cursor;
    bool app_keypad;
    bool backarrow_bs;
    bool bracketed_paste;
    bool focus_events;
    bool alternate;
    bool reverse_video;
    bool synchronized;
};

struct term_modes term_modes(const struct term *t);

enum term_input_status {
    TERM_INPUT_UNHANDLED = 0,
    TERM_INPUT_CONSUMED,
    TERM_INPUT_BYTES,
    TERM_INPUT_BUFFER_FULL,
};

struct term_input_result {
    size_t len;
    enum term_input_status status;
};

struct term_input_result term_input_encode_key(
    const struct term_keyboard_state *keyboard,
    const struct term_key_event *ev,
    const struct term_modes *modes,
    char *out, size_t cap
);

struct term_input_result term_input_encode_mouse(
    const struct term_mouse_event *ev,
    const struct term_modes *modes,
    char *out, size_t cap
);
```

---

## 4. Caller Migration Map

The table below documents every repository call site and its exact mapping to the revised API:

| Source File | Function / Operation Called | Headless API Equivalent | Notes |
| :--- | :--- | :--- | :--- |
| `src/session/session.c:46` | `term_feed(s->term, buf, len, s->onlcr ? TERM_FEED_ONLCR : 0)` | `term_feed(..., TERM_FEED_ONLCR)` | Preserved directly. |
| `src/session/session.c:276` | `term_resize(s->term, grid)` | `term_resize(s->term, (struct term_vector2i){ grid.x, grid.y })` | Returns `bool` so caller skips PTY resize on failure. |
| `src/terminal/terminal.c:512` | `term_set_cursor_default(term, style, steady)` | `term_set_cursor_default(term, style, steady)` | Preserved directly for config updates. |
| `src/terminal/view.c:484` | `term_history_text(term, 500)` | `term_history_text(term, 500)` | Preserved directly for AT-SPI screen readers. |
| `src/terminal/view.c:577` | `term_lock(term)` / `term_unlock(term)` | `term_lock(term)` / `term_unlock(term)` | Synchronizes external thread view reads with sequence lock. |
| `src/terminal/view.c:186` | `term_link_at(term, cell)` | `term_link_at(term, (struct term_vector2i){ cell.x, cell.y })` | Preserved directly. |
| `src/terminal/selection.c:37` | `term_select_start(term, cell, right_half, unit)` | `term_select_start(term, term_vec, right_half, unit)` | Preserved directly with units and half-cell flags. |
| `src/terminal/selection.c:16` | `term_select_extend(term, cell, right_half)` | `term_select_extend(term, term_vec, right_half)` | Preserved directly. |
| `src/terminal/selection.c:30` | `term_selection_text(term)` | `term_selection_text(term)` | Preserved directly. |
| `src/terminal/draw.c:83` | `term_snapshot_view(s)`, `term_snapshot_row(s, y)` | `term_snapshot_view(s)`, `term_snapshot_row(s, y)` | Preserved directly. |
| `src/render/` | `term_snapshot_row_selection(s, y)` | `term_snapshot_row_selection(s, y)` | Preserved directly. |
| `src/render/` | `term_view_color(v, cell->fg)` | `term_view_color(v, cell->fg)` | Preserved directly. |

---

## 5. Build System Integration and Standalone Target

### 5.1. Standalone Target (`subprojects/term/meson.build`)

`subprojects/term/meson.build` provides a standalone library definition with an explicit ISA option:

```meson
project('term_headless', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'b_ndebug=if-release'
  ]
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)
threads_dep = dependency('threads')

isa_opt = get_option('isa')
simd_args = []

if isa_opt == 'native'
  if cc.has_argument('-march=native')
    simd_args += ['-march=native']
  endif
elif isa_opt == 'avx512'
  # TERM_TEXT_AVX512 requires AVX512F, AVX512BW, and AVX512VBMI
  simd_args += ['-mavx512f', '-mavx512cd', '-mavx512bw', '-mavx512dq', '-mavx512vl', '-mavx512vbmi']
elif isa_opt == 'avx2'
  simd_args += ['-mavx2']
elif isa_opt != '' and isa_opt != 'none'
  simd_args += ['-march=' + isa_opt]
endif

term_sources = files(
  'core.c',
  'escape.c',
  'fastcsi.c',
  'gstore.c',
  'hist.c',
  'hist_view.c',
  'input.c',
  'palette.c',
  'screen.c',
  'selection.c',
  'snapshot.c',
  'term.c',
  'text.c',
  'text_run.c',
  'osc/clipboard.c',
  'osc/dispatch.c',
  'osc/link.c',
  'osc/misc.c'
)

term_headless_lib = both_libraries('term_headless',
  term_sources,
  include_directories: include_directories('.'),
  dependencies: [m_dep, threads_dep],
  c_args: simd_args,
  install: false
)
```

`subprojects/term/meson.options`:
```meson
option('isa', type: 'string', value: 'native',
       description: 'Compiler -march target for SIMD: native, avx512, avx2, or none')
```

### 5.2. Root Application Integration (`src/meson.build`)

In `src/meson.build`:
1. `term_lib` is defined by delegating directly to `subprojects/term/`:
   ```meson
   subdir('term')
   term_lib = term_headless_lib
   ```
2. `unicode_src` is separated into its own static library:
   ```meson
   unicode_lib = static_library(
       'wsap-unicode',
       unicode_src,
       include_directories: inc,
       install: false,
   )
   ```
3. `font_lib` and `ui_lib` link with `unicode_lib` without conflating it with `term_lib`.
4. In `tests/meson.build`, `term-test`, `term-input-test`, `spec-test`, `view-test`, `resize-property-test`, and `term-resize-failure-test` link directly with `term_headless_lib` to exercise the exact standalone boundaries.

---

## 6. Execution Phases

### Phase 0: Baseline Performance Capture
- Run benchmarks on target architectures (AVX-512, AVX2, NEON).
- Record min, median, and stddev across 5 repeated runs:
  - `meson test -C build --benchmark term-write-bench --verbose`
  - `meson test -C build --benchmark render-bench --verbose`

### Phase 1: Internal Include Path Normalization and Utilities Decoupling
- Normalize all internal includes in `subprojects/term/src/` by removing the `"term/"` directory prefix:
  - `cell.h:9`: `#include "term/term.h"` $\rightarrow$ `#include "term.h"`
  - `hist.c:1-2`: `#include "term/hist.h"` $\rightarrow$ `#include "hist.h"`
  - `hist.h:4-6`: `#include "term/cell.h"` $\rightarrow$ `#include "cell.h"`, etc.
  - `input.c:1`, `input.h:5`, `internal.h:4`, `palette.h:7`, `pen.h:4`, `term.c:1`, `text.c:9` $\rightarrow$ use relative paths.
- Add `subprojects/term/include/term/term_types.h` defining canonical `struct term_vector2i` and `struct term_rect2i`.
- Remove `#include "util/geom.h"` from `subprojects/term/`.
- Move gamma transfer functions (`srgb_to_linear`, `linear_to_srgb`) from `paint/blend.h` into `palette.c`. Link `-lm`.
- Internalize integer mix helpers (`hash64_step`, `hash64_finish`) into `internal.h`. Remove `#include "util/hash.h"`.
- Remove `#include "platform/platform.h"` from `screen.c`.
- Verify internal syntax probe:
  ```bash
  cc -std=c11 -Isubprojects/term/include/term -Isubprojects/term/include -fsyntax-only subprojects/term/src/cell.h
  ```

### Phase 2: Decouple Input Subsystem and Preserve Status Contracts
- Add `subprojects/term/include/term/term_input.h` defining `enum term_key`, `enum term_key_action`, `enum term_mod`, `struct term_keyboard_state`, and `struct term_input_result`.
- Remove `#include "input/input.h"` from `input.c` and `term.c`.
- Update `src/terminal/input.c` to translate app input events into `struct term_key_event` and `struct term_mouse_event`.

### Phase 3: Unicode Ownership and Static Inline Definitions
- Implement `utf8_encode()` and decode routines as `static inline` in `subprojects/term/include/term/unicode/utf8.h`.
- Implement `unicode_width()` and category queries in `subprojects/term/include/term/unicode/width.h` as `static inline` functions backed by local tables.
- Because routines are `static inline`, linking `libterm_headless` into the main application or external programs causes zero duplicate symbol collisions with `subprojects/term/include/term/unicode/`.
- Keep grapheme cluster allocations strictly scoped to `gstore.c` per terminal instance. Avoid global cluster stores.
- Remove `#include "unicode/..."` references pointing outside `subprojects/term/`.

### Phase 4: Standalone Build Validation and C11 Syntax
- Add `#include <stdalign.h>` to `text_run.c` and any headers using `alignas`.
- Add standalone `subprojects/term/meson.build` and `subprojects/term/meson.options`.
- Verify out-of-tree build: copy `subprojects/term/` to `/tmp/isolated_term` and build `libterm_headless.a` with zero external includes.

### Phase 5: Reconnect Application and Render Consumers
- Add explicit geometry conversion helpers (`term_vec_from_geom`, `geom_vec_from_term`) in `src/terminal/` boundary code.
- Update `src/session/` and `src/terminal/` according to the Caller Migration Map.
- Separate `unicode_src` in `src/meson.build` into `unicode_lib`, and set `term_lib = term_headless_lib`.
- Keep `src/render/` consuming snapshots via `term_snapshot_view()`, `term_snapshot_row()`, `term_snapshot_row_selection()`, and `term_view_color()`.

---

## 7. Verification and Acceptance Criteria

1. **Isolation Test**:
   Copy `subprojects/term/` to `/tmp/isolated_term`. Build `libterm_headless.a` and a minimal headless test executable using only system libraries (`-lm`, `-lpthread`). The build must succeed with zero external include flags.

2. **Core Unit Test Suite**:
   Run the terminal unit test suite:
   ```bash
   meson test -C build term-test
   meson test -C build term-input-test
   meson test -C build spec-test
   meson test -C build view-test
   meson test -C build resize-property-test
   meson test -C build term-resize-failure-test
   meson test -C build osc-test
   ```
   All tests must pass.

3. **Memory Safety Verification (ASan / UBSan)**:
   Configure and test under sanitizers:
   ```bash
   meson setup build-asan -Db_sanitize=address,undefined
   meson test -C build-asan term-test term-input-test spec-test view-test resize-property-test term-resize-failure-test osc-test
   ```
   All tests must pass cleanly.

4. **Benchmark Parity**:
   Run repeated measurements (minimum 5 runs; record min, median, stddev, and ISA):
   - `meson test -C build --benchmark term-write-bench --verbose`
   - `meson test -C build --benchmark render-bench --verbose` (including `flood-*` cases)
   Performance must match or exceed baseline throughput.

5. **OSC and Reply Validation**:
   - Verify that DSR cursor reports (`\x1b[6n`), device attributes (`DA`), Kitty keyboard queries, and OSC 4/10/11 color queries stream replies through `callbacks.reply`.
   - Verify that OSC 52 clipboard writes trigger `callbacks.clipboard`, while OSC 52 clipboard reads (`?`) are ignored.
