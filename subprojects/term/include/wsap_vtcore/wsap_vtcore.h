#ifndef WSAP_VTCORE_H
#define WSAP_VTCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wsap_vtcore_input.h"
#include "wsap_vtcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wsap_vtcore;
struct wsap_vtcore_snapshot;

#define WSAP_VTCORE_MAX_COLS 65535
#define WSAP_VTCORE_SCROLLBACK_DEFAULT 10000
#define WSAP_VTCORE_SCROLLBACK_MAX 1000000

#define WSAP_VTCORE_VERSION(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define WSAP_VTCORE_VERSION_MAJOR 0
#define WSAP_VTCORE_VERSION_MINOR 1
#define WSAP_VTCORE_VERSION_PATCH 0
#define WSAP_VTCORE_VERSION_CODE WSAP_VTCORE_VERSION(WSAP_VTCORE_VERSION_MAJOR, WSAP_VTCORE_VERSION_MINOR, WSAP_VTCORE_VERSION_PATCH)

typedef uint32_t wsap_vtcore_color;

#define WSAP_VTCORE_COLOR_SLOT 0x01000000u
#define WSAP_VTCORE_COLOR_SLOT_INDEX 0x1FFu
#define WSAP_VTCORE_COLOR_FG (WSAP_VTCORE_COLOR_SLOT | 0x100u)
#define WSAP_VTCORE_COLOR_BG (WSAP_VTCORE_COLOR_SLOT | 0x101u)

static inline wsap_vtcore_color wsap_vtcore_color_index(int n)
{
    return WSAP_VTCORE_COLOR_SLOT | ((uint32_t)n & 0xFFu);
}

static inline wsap_vtcore_color wsap_vtcore_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static inline bool wsap_vtcore_color_is_slot(wsap_vtcore_color c)
{
    return (c >> 24) == (WSAP_VTCORE_COLOR_SLOT >> 24);
}

WSAP_VTCORE_API bool wsap_vtcore_color_parse(const char *spec, uint32_t *argb);

struct wsap_vtcore_theme {
    uint32_t fg, bg, cursor;
    uint32_t ansi[16];
};

WSAP_VTCORE_API void wsap_vtcore_theme_default(struct wsap_vtcore_theme *theme);
WSAP_VTCORE_API void wsap_vtcore_theme_high_contrast(struct wsap_vtcore_theme *theme);
WSAP_VTCORE_API bool wsap_vtcore_theme_set_key(struct wsap_vtcore_theme *p, const char *key, const char *val);

enum wsap_vtcore_cursor_style {
    WSAP_VTCORE_CURSOR_BLOCK,
    WSAP_VTCORE_CURSOR_UNDERLINE,
    WSAP_VTCORE_CURSOR_BAR,
};

enum wsap_vtcore_mouse {
    WSAP_VTCORE_MOUSE_OFF = 0,
    WSAP_VTCORE_MOUSE_X10 = 9,
    WSAP_VTCORE_MOUSE_NORMAL = 1000,
    WSAP_VTCORE_MOUSE_BUTTON = 1002,
    WSAP_VTCORE_MOUSE_ANY = 1003,
};

enum wsap_vtcore_mouse_encoding {
    WSAP_VTCORE_MOUSE_ENC_X10 = 0,
    WSAP_VTCORE_MOUSE_ENC_UTF8 = 1005,
    WSAP_VTCORE_MOUSE_ENC_SGR = 1006,
    WSAP_VTCORE_MOUSE_ENC_URXVT = 1015,
    WSAP_VTCORE_MOUSE_ENC_SGR_PIXELS = 1016,
};

/* Callbacks run on the calling thread while the terminal is locked.
 * Payload pointers are valid only until the callback returns.
 * Do not feed, resize, free, or otherwise change the terminal in a callback.
 * Queue that work until the enclosing call returns. Do not wait for another
 * thread that needs this terminal. The lock alone does not make parser re-entry safe.
 */
struct wsap_vtcore_callbacks {
    void *ud;
    void (*reply)(void *ud, const char *bytes, size_t len);
    void (*title)(void *ud, const char *title);
    void (*clipboard)(void *ud, const char *selection, const char *data, size_t len);
    void (*notify)(void *ud, const char *title, const char *body);
    void (*bell)(void *ud);
    void (*trace)(void *ud, const char *line);
};

/* Zero-initialize options. A zero size selects 80 by 24; zero scrollback
 * selects WSAP_VTCORE_SCROLLBACK_DEFAULT; zero theme selects built-in colors.
 * A null name selects "wsap_vtcore". Other zero fields select defaults.
 */
struct wsap_vtcore_options {
    struct wsap_vtcore_vector2i size;
    int scrollback;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_steady;
    const char *name;
    struct wsap_vtcore_callbacks callbacks;
    uint32_t reserved[8];
};

WSAP_VTCORE_API const char *wsap_vtcore_unicode_version(void);
WSAP_VTCORE_API unsigned wsap_vtcore_version(void);

/* Return the length of the leading run of bytes from 0x20 up to, but not
 * including, hi. Use 0x7f for printable ASCII. The result is the same at
 * every SIMD level.
 */
WSAP_VTCORE_API size_t wsap_vtcore_scan_printable_span(const char *buf, size_t len, unsigned char hi);

WSAP_VTCORE_API const char *wsap_vtcore_isa(void);

/* Returns NULL for invalid options or allocation failure. The core copies the
 * options' theme, callbacks and name; callback userdata remains caller-owned.
 */
WSAP_VTCORE_API struct wsap_vtcore *wsap_vtcore_new(const struct wsap_vtcore_options *opt);
/* NULL is allowed. Stop all users before freeing the terminal. */
WSAP_VTCORE_API void wsap_vtcore_free(struct wsap_vtcore *t);

/* Terminal calls serialize on one terminal. Group calls with these locks.
 * Nesting is allowed on one thread. Balance locks on that same thread.
 * Do not call unlock without a matching lock. NULL is allowed for both calls.
 */
WSAP_VTCORE_API void wsap_vtcore_lock(struct wsap_vtcore *t);
WSAP_VTCORE_API void wsap_vtcore_unlock(struct wsap_vtcore *t);

/* Dimensions must be positive. Width must not exceed WSAP_VTCORE_MAX_COLS.
 * Return false for invalid input or allocation failure. Keep the old state.
 */
WSAP_VTCORE_API bool wsap_vtcore_resize(struct wsap_vtcore *t, struct wsap_vtcore_vector2i size);

WSAP_VTCORE_API void wsap_vtcore_set_theme(struct wsap_vtcore *t, const struct wsap_vtcore_theme *theme);

WSAP_VTCORE_API void wsap_vtcore_set_cursor_default(struct wsap_vtcore *t, enum wsap_vtcore_cursor_style style, bool steady);

enum wsap_vtcore_feed_flags {
    WSAP_VTCORE_FEED_ONLCR = 1u << 0,
};

/* False means invalid arguments or unsupported flags, before consuming bytes.
 * True means the full supplied byte span was accepted; parser and internal
 * allocation failures are not reported by this return value.
 */
WSAP_VTCORE_API bool wsap_vtcore_feed(struct wsap_vtcore *t, const char *bytes, size_t len, unsigned flags);

WSAP_VTCORE_API void wsap_vtcore_focus(struct wsap_vtcore *t, bool focused);
/* Zero disables history. Reject invalid limits. Keep state on allocation failure. */
WSAP_VTCORE_API bool wsap_vtcore_set_scrollback_limit(struct wsap_vtcore *t, int lines);

struct wsap_vtcore_modes {
    unsigned keyboard;
    enum wsap_vtcore_mouse mouse;
    enum wsap_vtcore_mouse_encoding mouse_encoding;
    struct wsap_vtcore_vector2i grid;
    bool app_cursor;
    bool app_keypad;
    bool backarrow_bs;
    bool bracketed_paste;
    bool focus_events;
    bool alternate;
    bool reverse_video;
    bool synchronized;
};

struct wsap_vtcore_cursor {
    struct wsap_vtcore_vector2i pos;
    enum wsap_vtcore_cursor_style style;
    bool visible;
    bool blink;
};

WSAP_VTCORE_API struct wsap_vtcore_vector2i wsap_vtcore_size(const struct wsap_vtcore *t);
WSAP_VTCORE_API struct wsap_vtcore_modes wsap_vtcore_modes(const struct wsap_vtcore *t);

WSAP_VTCORE_API struct wsap_vtcore_input_result wsap_vtcore_input_encode_key(
    const struct wsap_vtcore_keyboard_state *keyboard,
    const struct wsap_vtcore_key_event *ev,
    const struct wsap_vtcore_modes *modes,
    char *buf, size_t capacity
);

WSAP_VTCORE_API struct wsap_vtcore_input_result wsap_vtcore_input_encode_mouse(
    const struct wsap_vtcore_mouse_event *ev,
    const struct wsap_vtcore_modes *modes,
    char *buf, size_t capacity
);

#define WSAP_VTCORE_PASTE_EXTRA 12

WSAP_VTCORE_API struct wsap_vtcore_input_result wsap_vtcore_input_encode_paste(
    const struct wsap_vtcore_modes *modes,
    const char *text, size_t len,
    char *buf, size_t capacity
);
WSAP_VTCORE_API struct wsap_vtcore_cursor wsap_vtcore_cursor(const struct wsap_vtcore *t);
/* Returned strings belong to the caller. Release them with text_free.
 * NULL means no value, invalid input, or allocation failure.
 */
WSAP_VTCORE_API char *wsap_vtcore_cwd(const struct wsap_vtcore *t);
WSAP_VTCORE_API char *wsap_vtcore_pointer_shape(const struct wsap_vtcore *t);
WSAP_VTCORE_API void wsap_vtcore_text_free(char *text);

struct wsap_vtcore_scrollback {
    int offset;
    int total;
};

WSAP_VTCORE_API struct wsap_vtcore_scrollback wsap_vtcore_scrollback(const struct wsap_vtcore *t);
WSAP_VTCORE_API void wsap_vtcore_scroll(struct wsap_vtcore *t, int lines);
WSAP_VTCORE_API void wsap_vtcore_scroll_to(struct wsap_vtcore *t, int offset);

enum wsap_vtcore_select_unit {
    WSAP_VTCORE_SELECT_CELL,
    WSAP_VTCORE_SELECT_WORD,
    WSAP_VTCORE_SELECT_LINE,
};

WSAP_VTCORE_API bool wsap_vtcore_select_start(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half,
                       enum wsap_vtcore_select_unit unit);
WSAP_VTCORE_API bool wsap_vtcore_select_extend(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half);
WSAP_VTCORE_API void wsap_vtcore_select_clear(struct wsap_vtcore *t);
WSAP_VTCORE_API bool wsap_vtcore_has_selection(const struct wsap_vtcore *t);
WSAP_VTCORE_API char *wsap_vtcore_selection_text(const struct wsap_vtcore *t);
WSAP_VTCORE_API char *wsap_vtcore_link_at(const struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell);

enum wsap_vtcore_attr {
    WSAP_VTCORE_ATTR_BOLD = 1u << 0,
    WSAP_VTCORE_ATTR_ITALIC = 1u << 1,
    WSAP_VTCORE_ATTR_UNDERLINE = 1u << 2,
    WSAP_VTCORE_ATTR_STRIKE = 1u << 3,
    WSAP_VTCORE_ATTR_OVERLINE = 1u << 4,
    WSAP_VTCORE_ATTR_EMOJI = 1u << 5,
    WSAP_VTCORE_ATTR_TEXT = 1u << 6,
    WSAP_VTCORE_ATTR_FAINT = 1u << 10,
    WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE = 1u << 11,
    WSAP_VTCORE_ATTR_CURLY_UNDERLINE = 1u << 12,
    WSAP_VTCORE_ATTR_DOTTED_UNDERLINE = 1u << 13,
    WSAP_VTCORE_ATTR_DASHED_UNDERLINE = 1u << 14,
    WSAP_VTCORE_ATTR_LINK = 1u << 15,
};

#define WSAP_VTCORE_ATTR_UNDERLINE_MASK \
    (WSAP_VTCORE_ATTR_UNDERLINE | WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE | WSAP_VTCORE_ATTR_CURLY_UNDERLINE | \
     WSAP_VTCORE_ATTR_DOTTED_UNDERLINE | WSAP_VTCORE_ATTR_DASHED_UNDERLINE)

enum wsap_vtcore_underline {
    WSAP_VTCORE_UNDERLINE_NONE,
    WSAP_VTCORE_UNDERLINE_SINGLE,
    WSAP_VTCORE_UNDERLINE_DOUBLE,
    WSAP_VTCORE_UNDERLINE_CURLY,
    WSAP_VTCORE_UNDERLINE_DOTTED,
    WSAP_VTCORE_UNDERLINE_DASHED,
};

static inline enum wsap_vtcore_underline wsap_vtcore_attr_underline(uint16_t attrs)
{
    /* Use count trailing zeros for branchless style decoding. */
    uint32_t adv = attrs & (WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE | WSAP_VTCORE_ATTR_CURLY_UNDERLINE |
                            WSAP_VTCORE_ATTR_DOTTED_UNDERLINE | WSAP_VTCORE_ATTR_DASHED_UNDERLINE);
#if defined(__GNUC__) || defined(__clang__)
    if (adv)
        return (enum wsap_vtcore_underline)(__builtin_ctz(adv) - 9);
#else
    if (adv) {
        if (attrs & WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE) return WSAP_VTCORE_UNDERLINE_DOUBLE;
        if (attrs & WSAP_VTCORE_ATTR_CURLY_UNDERLINE) return WSAP_VTCORE_UNDERLINE_CURLY;
        if (attrs & WSAP_VTCORE_ATTR_DOTTED_UNDERLINE) return WSAP_VTCORE_UNDERLINE_DOTTED;
        return WSAP_VTCORE_UNDERLINE_DASHED;
    }
#endif
    return attrs & WSAP_VTCORE_ATTR_UNDERLINE ? WSAP_VTCORE_UNDERLINE_SINGLE : WSAP_VTCORE_UNDERLINE_NONE;
}

#define WSAP_VTCORE_TEXT_CLUSTER 0x80000000u

struct wsap_vtcore_cell {
    uint32_t text;
    wsap_vtcore_color fg, bg;
    uint16_t attrs;
    uint8_t width;
    uint8_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(struct wsap_vtcore_cell) == 16, "four cells fill one 64-byte line");
#else
_Static_assert(sizeof(struct wsap_vtcore_cell) == 16, "four cells fill one 64-byte line");
#endif

/* A snapshot owns its copied rows, palette, and grapheme data.
 * The data stays valid even after the source terminal is freed.
 * Getter pointers are valid until the next update attempt or snapshot_free.
 * Readers can share a snapshot if no thread updates or frees it.
 */
WSAP_VTCORE_API struct wsap_vtcore_snapshot *wsap_vtcore_snapshot_new(void);
WSAP_VTCORE_API void wsap_vtcore_snapshot_free(struct wsap_vtcore_snapshot *s);

/* Copy the terminal view. A snapshot can capture different terminals.
 * On failure, keep the prior values. Re-fetch getter pointers after any attempt.
 * Do not update a snapshot while another thread reads or updates it.
 */
WSAP_VTCORE_API bool wsap_vtcore_snapshot_update(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t);

enum {
    WSAP_VTCORE_PALETTE_FG = 256,
    WSAP_VTCORE_PALETTE_BG = 257,
    WSAP_VTCORE_PALETTE_CURSOR = 258,
    WSAP_VTCORE_PALETTE_SIZE = 259,
};

struct wsap_vtcore_view {
    struct wsap_vtcore_vector2i size;
    struct wsap_vtcore_cursor cursor;
    int scroll_offset;
    int scrolled;
    bool reverse_video;
    uint64_t version;
    uint64_t palette_version;
    const uint32_t *palette;
};

WSAP_VTCORE_API const struct wsap_vtcore_view *wsap_vtcore_snapshot_view(const struct wsap_vtcore_snapshot *s);
WSAP_VTCORE_API const struct wsap_vtcore_cell *wsap_vtcore_snapshot_row(const struct wsap_vtcore_snapshot *s, int y);
WSAP_VTCORE_API uint64_t wsap_vtcore_snapshot_row_key(const struct wsap_vtcore_snapshot *s, int y);

WSAP_VTCORE_API int wsap_vtcore_history_rows(const struct wsap_vtcore *t);
WSAP_VTCORE_API char *wsap_vtcore_history_text(struct wsap_vtcore *t, int max_rows);
WSAP_VTCORE_API struct wsap_vtcore_span wsap_vtcore_snapshot_row_selection(const struct wsap_vtcore_snapshot *s, int y);

/* Return the full code point count. Copy at most cap code points to cps.
 * Use NULL and zero capacity to query the count. Invalid handles return zero.
 * Cluster handles belong to one snapshot capture. Do not retain them across updates.
 */
WSAP_VTCORE_API int wsap_vtcore_snapshot_cluster(const struct wsap_vtcore_snapshot *s, uint32_t text, uint32_t *cps, int cap);
WSAP_VTCORE_API const uint32_t *wsap_vtcore_snapshot_cluster_direct(const struct wsap_vtcore_snapshot *s, uint32_t text, int *out_len);

/* Literal colors pass through unchanged. Invalid slots or a missing palette
 * resolve to zero.
 */
static inline uint32_t wsap_vtcore_view_color(const struct wsap_vtcore_view *v, wsap_vtcore_color c)
{
    if (!wsap_vtcore_color_is_slot(c))
        return c;
    uint32_t index = c & WSAP_VTCORE_COLOR_SLOT_INDEX;
    if (!v || !v->palette || index >= WSAP_VTCORE_PALETTE_SIZE)
        return 0;
    return v->palette[index];
}

#ifdef __cplusplus
}
#endif

#endif
