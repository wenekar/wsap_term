#ifndef WSAP_VTCORE_INTERNAL_H
#define WSAP_VTCORE_INTERNAL_H

#include "wsap_vtcore.h"
#include "cell.h"
#include "csi.h"
#include "gstore.h"
#include "hist.h"
#include "hist_view.h"
#include "palette.h"
#include "pen.h"
#include "text_run.h"
#include "compat.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define HASH64_BASIS UINT64_C(0x9E3779B97F4A7C15)
#define KNUTH_HASH_MULT UINT32_C(2654435761)

static inline uint64_t hash64_step(uint64_t hash, uint32_t value)
{
    hash ^= value;
    hash *= HASH64_BASIS;
    return hash ^ (hash >> 29);
}

static inline uint64_t hash64_finish(uint64_t hash)
{
    hash ^= hash >> 32;
    hash *= UINT64_C(0xD6E8FEB86659FD93);
    hash ^= hash >> 32;
    return hash;
}

#define KEYBOARD_STACK_SIZE 32

struct keyboard_mode {
    unsigned flags, saved[KEYBOARD_STACK_SIZE], depth;
};

/* What DECSC saves, as in xterm: the cursor, the pen, origin mode, the
 * pending wrap, and the character sets. */
struct wsap_vtcore_saved_cursor {
    int x, y;
    uint32_t fg, bg, ul;
    char charsets[2];
    uint8_t underline;
    uint8_t charset_active;
    bool bold : 1;
    bool faint : 1;
    bool italic : 1;
    bool strike : 1;
    bool overline : 1;
    bool inverse : 1;
    bool origin : 1;
    bool pending_wrap : 1;
};

/*
 * A row stores cells, text, or default-pen ASCII. A text row uses the history
 * format. An ASCII row stores only its pointer and length. A screen read or
 * edit sets its text fields first. Other changes settle text into cells.
 */
enum { ROW_CELLS, ROW_TEXT, ROW_ASCII };
enum { RF_WRAPPED = 1, RF_WIDE = 4, RF_CLUSTER = 8, RF_TAIL_PADDED = 16 };

struct srow {
    struct cell *cells;                  /* stride >= cols, from a per-screen slab */
    uint8_t *ascii;
    struct text_row text;                /* TEXT: the content; len == text.cells, or cols with a wrap pad */
    uint64_t uid;                        /* new id when the row scrolls in or clears */
    uint32_t version;                    /* bumped on every change */
    uint16_t len;
    uint8_t kind, flags;
};

struct screen {
    struct srow *slot;
    struct cell *cells_slab;
    uint8_t *text_slab;
    size_t cells_cap, text_cap;
    /* Without history: the line of row 0, counted as rows leave the top. */
    uint64_t top_id;
    int rows, cols, top;
    int stride;
    /* Capacity of the buffers above. A resize reuses them. */
    int slot_cap;
    uint32_t top_seg;
};

struct wsap_vtcore_sel_point {
    uint64_t line_id;
    uint32_t cell_off;
};

/* A buffer that only grows, so work that repeats does not allocate. */
struct grow_buf {
    void *data;
    size_t cap;
};

/* What a resize needs besides the screens. It keeps its buffers, so a
 * resize allocates nothing once the terminal has seen its sizes. */
struct resize_scratch {
    struct grow_buf row, rows, cells, line, pull, rest, lens;
};


struct wsap_vtcore {
    /* The writer section. A waiting thread sleeps on it. */
    wsap_vtcore_mutex write_lock;
    _Atomic uint64_t writer_tid;
    uint32_t write_depth;
    int cols;
    int rows;
    int cursor_x;
    int cursor_y;
    int scroll_top, scroll_bottom;

    uint32_t cur_fg;
    uint32_t cur_bg;
    uint32_t cur_ul;
    uint32_t last_char;

    uint32_t utf8_state;
    uint32_t utf8_codep;
    struct width_cache text_width;

    enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_DCS, ST_STR_ESC, ST_CHARSET,
           ST_DCS_ESC, ST_IGNORE, ST_IGNORE_ESC, ST_DOCS, ST_DECALN } state;
    struct csi_parser csi_parser;

    uint16_t cur_link;
    char charsets[2];
    uint8_t underline;
    uint8_t decsace;
    char charset_designation;
    bool graphics;

    bool bold : 1;
    bool faint : 1;
    bool italic : 1;
    bool strike : 1;
    bool overline : 1;
    bool inverse : 1;
    bool pending_wrap : 1;
    bool autowrap : 1;
    bool insert_mode : 1;
    bool origin_mode : 1;
    bool reverse_video : 1;
    bool synchronized_output : 1;
    bool alternate : 1;
    bool application_cursor : 1;
    bool application_keypad : 1;
    bool lr_margin_mode : 1;
    bool backarrow_sends_bs : 1;
    bool focus_events : 1;
    bool cursor_visible : 1;
    bool cursor_blink : 1;
    bool default_cursor_blink : 1;
    bool selection : 1;
    bool bracketed_paste : 1;
    bool lnm : 1;
    bool transport_newline : 1;
    bool str_truncated : 1;
    bool has_prompt : 1;

    struct screen scr;
    struct screen alt_scr;
    struct screen spare;                 /* the buffers a resize builds into */
    struct resize_scratch resize;
    struct grow_buf ascii_ends;
    struct grow_buf ascii_tail;
    struct wsap_vtcore_saved_cursor normal_saved_cursor;
    struct wsap_vtcore_saved_cursor normal_saved_decsc;
    int normal_cols, normal_rows;
    bool alt_allocated;
    bool normal_cursor_visible;
    uint8_t normal_cursor_style;
    bool normal_cursor_blink;

    struct hist hist;
    struct hist_view view;
    uint64_t next_row_uid;
    uint64_t full_gen;
    int prompt_y;
    int left_margin, right_margin;
    struct wsap_vtcore_sel_point sel_start, sel_end;
    struct wsap_vtcore_sel_point sel_anchor_first, sel_anchor_last;
    enum wsap_vtcore_select_unit select_unit;

    int mouse_mode, mouse_encoding;
    enum wsap_vtcore_cursor_style cursor_style;
    enum wsap_vtcore_cursor_style default_cursor_style;
    int charset_active;
    int osc_len;
    int dcs_len;
    int sb_capacity;

    struct keyboard_mode keyboard[2];
    /* The DECSC slot. Cold: it stays out of the hot fields above. */
    struct wsap_vtcore_saved_cursor saved;

    uint8_t *tabs;
    struct cell *view_row_scratch;
    int tabs_cap;
    int view_scratch_cap;
    bool custom_tabs;

    char osc_buf[256];
    /* The host's name. A DCS sequence can change tcap_name, not this. */
    char name[64];
    char tcap_name[64];

    char *osc_heap;
    size_t osc_heap_cap;
    char *dcs_buf;
    size_t dcs_cap;
    void *links;
    char *cwd;
    char *pointer_shape;

    struct wsap_vtcore_callbacks cb;
    struct palette palette;
    struct gstore *gstore;
    /* Unique for the life of the process. A snapshot compares it, not the
     * address, because a new terminal can reuse a freed one's memory. */
    uint64_t serial;
};

static inline void reset_sgr(struct wsap_vtcore *t)
{
    t->cur_fg = WSAP_VTCORE_COLOR_FG;
    t->cur_bg = WSAP_VTCORE_COLOR_BG;
    t->bold = false;
    t->faint = false;
    t->italic = false;
    t->underline = 0;
    t->strike = false;
    t->overline = false;
    t->inverse = false;
    t->cur_ul = 0;
}

/* The DECSC slot before any save: home, the default pen, and ASCII. */
static inline struct wsap_vtcore_saved_cursor wsap_vtcore_saved_cursor_initial(void)
{
    return (struct wsap_vtcore_saved_cursor){
        .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG, .charsets = { 'B', 'B' },
    };
}

static inline struct wsap_vtcore_saved_cursor wsap_vtcore_cursor_save(const struct wsap_vtcore *t)
{
    return (struct wsap_vtcore_saved_cursor){
        .x = t->cursor_x, .y = t->cursor_y,
        .fg = t->cur_fg, .bg = t->cur_bg, .ul = t->cur_ul,
        .charsets = { t->charsets[0], t->charsets[1] },
        .underline = t->underline,
        .charset_active = (uint8_t)t->charset_active,
        .bold = t->bold, .faint = t->faint, .italic = t->italic,
        .strike = t->strike, .overline = t->overline, .inverse = t->inverse,
        .origin = t->origin_mode, .pending_wrap = t->pending_wrap,
    };
}

/* Take the pen, origin mode, and character sets from s. The caller places
 * the cursor. */
static inline void wsap_vtcore_cursor_apply(struct wsap_vtcore *t, const struct wsap_vtcore_saved_cursor *s)
{
    t->cur_fg = s->fg;
    t->cur_bg = s->bg;
    t->cur_ul = s->ul;
    t->underline = s->underline;
    t->bold = s->bold;
    t->faint = s->faint;
    t->italic = s->italic;
    t->strike = s->strike;
    t->overline = s->overline;
    t->inverse = s->inverse;
    t->origin_mode = s->origin;
    t->charsets[0] = s->charsets[0];
    t->charsets[1] = s->charsets[1];
    t->charset_active = s->charset_active;
    t->graphics = t->charsets[t->charset_active] == '0';
}

static inline void send_response(struct wsap_vtcore *t, const char *str)
{
    if (t && t->cb.reply)
        t->cb.reply(t->cb.ud, str, strlen(str));
}
static inline int wsap_vtcore_hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool wsap_vtcore_lnm(const struct wsap_vtcore *t)
{
    return t->lnm || t->transport_newline;
}

static inline bool mode_is_mouse(int m)
{
    return m == 9 || (m >= 1000 && m <= 1003);
}

static inline bool mode_is_mouse_enc(int m)
{
    return m == 1005 || m == 1006 || m == 1015 || m == 1016;
}

static const uint16_t wsap_vtcore_ul_flags[] = {
    0, CELL_UNDERLINE, CELL_DOUBLE_UNDERLINE,
    CELL_CURLY_UNDERLINE, CELL_DOTTED_UNDERLINE,
    CELL_DASHED_UNDERLINE,
};

static inline uint16_t wsap_vtcore_current_flags(const struct wsap_vtcore *t)
{
    uint16_t f = 0;
    if (t->bold) f |= CELL_BOLD;
    if (t->faint) f |= CELL_FAINT;
    if (t->italic) f |= CELL_ITALIC;
    if (t->underline < sizeof(wsap_vtcore_ul_flags) / sizeof(wsap_vtcore_ul_flags[0]))
        f |= wsap_vtcore_ul_flags[t->underline];
    if (t->strike) f |= CELL_STRIKE;
    if (t->overline) f |= CELL_OVERLINE;
    if (t->cur_link) f |= CELL_LINK;
    return f;
}

static inline uint32_t wsap_vtcore_flags_pad(const struct wsap_vtcore *t)
{
    return wsap_vtcore_current_flags(t) | ((uint32_t)t->cur_link << 16);
}

static inline struct text_pen wsap_vtcore_text_pen(const struct wsap_vtcore *t)
{
    return (struct text_pen){
        .fg = WSAP_VTCORE_EXPECT(t->inverse, 0) ? t->cur_bg : t->cur_fg,
        .bg = WSAP_VTCORE_EXPECT(t->inverse, 0) ? t->cur_fg : t->cur_bg,
        .sl = wsap_vtcore_flags_pad(t),
    };
}

static inline void wsap_vtcore_tabs_clear(struct wsap_vtcore *t)
{
    memset(t->tabs, 0, (size_t)t->tabs_cap);
    t->custom_tabs = true;
}

static inline void wsap_vtcore_tabs_default(struct wsap_vtcore *t)
{
    for (int c = 8; c < t->tabs_cap; c += 8) t->tabs[c] = 1;
    t->custom_tabs = false;
}

static inline bool wsap_vtcore_str_was_truncated(struct wsap_vtcore *t)
{
    bool truncated = t->str_truncated;
    t->str_truncated = false;
    return truncated;
}

/* Every thread has its own anchor, so its address names the thread. */
extern _Thread_local char wsap_vtcore_thread_anchor;

static inline void wsap_vtcore_write_begin(struct wsap_vtcore *t)
{
    if (!t) return;
    uint64_t self = (uint64_t)(uintptr_t)&wsap_vtcore_thread_anchor;
    if (atomic_load_explicit(&t->writer_tid, memory_order_relaxed) == self) {
        t->write_depth++;
        return;
    }
    wsap_vtcore_mutex_lock(&t->write_lock);
    atomic_store_explicit(&t->writer_tid, self, memory_order_relaxed);
    t->write_depth = 1;
}

static inline void wsap_vtcore_write_end(struct wsap_vtcore *t)
{
    if (!t) return;
    if (--t->write_depth == 0) {
        atomic_store_explicit(&t->writer_tid, 0, memory_order_relaxed);
        wsap_vtcore_mutex_unlock(&t->write_lock);
    }
}

#endif
