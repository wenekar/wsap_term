#ifndef WSAP_VTCORE_H
#define WSAP_VTCORE_H

/*
 * Terminal emulator core.
 *
 * A struct wsap_vtcore parses the bytes that a program writes and keeps the
 * screen, the history, and the modes. The application feeds bytes with
 * wsap_vtcore_feed() and reads state with the getters below. A renderer never
 * reads the terminal directly: it copies the viewport into a struct
 * wsap_vtcore_snapshot and paints from that copy.
 *
 * All functions take the terminal as the first argument. Functions that take a
 * const struct wsap_vtcore do not change what it shows. Selection, history,
 * link, directory, and pointer-shape queries return owned strings; release
 * them with wsap_vtcore_text_free().
 *
 * Threads. Any thread can call any function. Each call that reads or changes a
 * terminal enters its writer section, so calls on one terminal run one at a
 * time. A thread that waits for the section sleeps. On POSIX the section is a
 * mutex with priority inheritance where the platform has it. A snapshot
 * belongs to one thread at a time. wsap_vtcore_free() is the last call on a
 * terminal.
 */

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

/* A row holds at most this many cells. */
#define WSAP_VTCORE_MAX_COLS 65535
/* The history limit when wsap_vtcore_options.scrollback is 0. */
#define WSAP_VTCORE_SCROLLBACK_DEFAULT 10000
/* The largest wsap_vtcore_options.scrollback. */
#define WSAP_VTCORE_SCROLLBACK_MAX 1000000

/*
 * The version of this header. A host compares WSAP_VTCORE_VERSION_CODE with
 * wsap_vtcore_version() to check that the library matches the header.
 */
#define WSAP_VTCORE_VERSION(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define WSAP_VTCORE_VERSION_MAJOR 0
#define WSAP_VTCORE_VERSION_MINOR 1
#define WSAP_VTCORE_VERSION_PATCH 0
#define WSAP_VTCORE_VERSION_CODE WSAP_VTCORE_VERSION(WSAP_VTCORE_VERSION_MAJOR, WSAP_VTCORE_VERSION_MINOR, WSAP_VTCORE_VERSION_PATCH)

/*
 * Colors. A wsap_vtcore_color is either a direct color (alpha 0xFF, then RGB)
 * or a palette slot: an index 0-255, the default foreground, or the default
 * background. A slot resolves when a snapshot is painted, so a theme change
 * recolors text that is already on screen.
 */
typedef uint32_t wsap_vtcore_color;

#define WSAP_VTCORE_COLOR_SLOT 0x01000000u
/* The palette index of a slot; see struct wsap_vtcore_view.palette. */
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

/* Parse an X11 or OSC color specification ("#rgb", "rgb:r/g/b", ...). */
WSAP_VTCORE_API bool wsap_vtcore_color_parse(const char *spec, uint32_t *argb);

/* The colors a terminal starts with. All values are ARGB. An all-zero theme
 * selects the built-in theme. */
struct wsap_vtcore_theme {
    uint32_t fg, bg, cursor;
    uint32_t ansi[16];
};

/* Fill theme with the built-in theme. */
WSAP_VTCORE_API void wsap_vtcore_theme_default(struct wsap_vtcore_theme *theme);
/* Fill theme with the high-contrast theme. */
WSAP_VTCORE_API void wsap_vtcore_theme_high_contrast(struct wsap_vtcore_theme *theme);
/* Sets one palette key: foreground, background, or color0 to color15.
 * An empty value resets the key to the built-in theme. False, with no
 * change, for an unknown key or a bad color. */
WSAP_VTCORE_API bool wsap_vtcore_theme_set_key(struct wsap_vtcore_theme *p, const char *key, const char *val);

enum wsap_vtcore_cursor_style {
    WSAP_VTCORE_CURSOR_BLOCK,
    WSAP_VTCORE_CURSOR_UNDERLINE,
    WSAP_VTCORE_CURSOR_BAR,
};

/* Mouse reporting modes and encodings. The values are the DEC mode numbers. */
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

/*
 * Callbacks. They run inside wsap_vtcore_feed() and wsap_vtcore_focus(), on
 * the thread that calls those functions. A callback must not call back into
 * the terminal. String arguments are valid only during the call.
 */
struct wsap_vtcore_callbacks {
    void *ud;
    /* Bytes the terminal sends back to the program (replies to queries). */
    void (*reply)(void *ud, const char *bytes, size_t len);
    void (*title)(void *ud, const char *title);
    /*
     * OSC 52 clipboard callback.
     * selection names the target clipboard buffer(s), commonly:
     *   "c" - clipboard (system clipboard)
     *   "p" - primary selection (X11 primary)
     *   "q" - secondary selection
     *   "s" - select selection
     *   "0".."7" - cut buffers 0 to 7
     * A parameter may contain multiple targets (e.g. "cp").
     * data contains decoded bytes of length len.
     */
    void (*clipboard)(void *ud, const char *selection, const char *data, size_t len);
    void (*notify)(void *ud, const char *title, const char *body);
    void (*bell)(void *ud);
    /* Parser trace lines for debugging. NULL turns tracing off. The line has no newline. */
    void (*trace)(void *ud, const char *line);
};

/*
 * Options for wsap_vtcore_new(). An all-zero value is valid. A size component
 * of 0 selects 80 columns or 24 rows. A negative component, or columns above
 * WSAP_VTCORE_MAX_COLS, makes wsap_vtcore_new() fail. A scrollback below 0 or
 * above WSAP_VTCORE_SCROLLBACK_MAX fails too. cursor_style must be a value of
 * enum wsap_vtcore_cursor_style.
 */
struct wsap_vtcore_options {
    struct wsap_vtcore_vector2i size;  /* columns and rows */
    int scrollback;             /* history lines; 0 selects WSAP_VTCORE_SCROLLBACK_DEFAULT */
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_steady;         /* false: the cursor blinks */
    /*
     * The terminal's name. Programs read it with XTVERSION and XTGETTCAP.
     * It is also the title of a notification that has none. NULL or ""
     * selects "wsap_vtcore". Otherwise 1 to 63 printable ASCII bytes, or
     * wsap_vtcore_new() returns NULL. wsap_vtcore_new() copies the name.
     */
    const char *name;
    struct wsap_vtcore_callbacks callbacks;
    /* Must be zero. wsap_vtcore_new() returns NULL if any word is nonzero, so a
     * later version can give a word a meaning. */
    uint32_t reserved[8];
};

/* The Unicode version of the width tables, such as "18.0.0". A host can
 * pass it to programs as UNICODE_VERSION. */
WSAP_VTCORE_API const char *wsap_vtcore_unicode_version(void);
/* The WSAP_VTCORE_VERSION_CODE the library was built with. */
WSAP_VTCORE_API unsigned wsap_vtcore_version(void);

/* The length of the prefix of buf made of bytes in [0x20, hi). With hi
 * 0x7f, it finds a run of printable ASCII in UTF-8 text, so a host can
 * skip shaping for it. Any thread can call it. */
WSAP_VTCORE_API size_t wsap_vtcore_scan_printable_span(const char *buf, size_t len, unsigned char hi);

/* The instruction set level of the running code: "base", "avx2", "avx512", or
 * "neon". On x86-64, the first wsap_vtcore_new() picks the highest level the
 * CPU supports. Set WSAP_VTCORE_ISA to a lower level to force it, for tests
 * and comparisons. */
WSAP_VTCORE_API const char *wsap_vtcore_isa(void);

/* NULL when memory runs out or an option is invalid. */
WSAP_VTCORE_API struct wsap_vtcore *wsap_vtcore_new(const struct wsap_vtcore_options *opt);
WSAP_VTCORE_API void wsap_vtcore_free(struct wsap_vtcore *t);

/* Hold the writer section across several calls, so that no other thread
 * changes the terminal between them. The section is a mutex, and a waiting
 * thread sleeps. Keep it short. The thread that holds it can call every
 * function, and it can enter the section again. Each wsap_vtcore_lock() needs
 * one wsap_vtcore_unlock(). */
WSAP_VTCORE_API void wsap_vtcore_lock(struct wsap_vtcore *t);
WSAP_VTCORE_API void wsap_vtcore_unlock(struct wsap_vtcore *t);

/* Change the size in cells. Returns false, and the terminal is unchanged,
 * when a component is below 1, when cols is above WSAP_VTCORE_MAX_COLS, or when
 * memory runs out. The screen reflows to the new width. */
WSAP_VTCORE_API bool wsap_vtcore_resize(struct wsap_vtcore *t, struct wsap_vtcore_vector2i size);

/* Replace the theme. A different theme also drops the colors that the
 * program set with OSC; the same theme changes nothing. */
WSAP_VTCORE_API void wsap_vtcore_set_theme(struct wsap_vtcore *t, const struct wsap_vtcore_theme *theme);

/* The cursor style that DECSCUSR 0 and a reset restore. */
WSAP_VTCORE_API void wsap_vtcore_set_cursor_default(struct wsap_vtcore *t, enum wsap_vtcore_cursor_style style, bool steady);

/*
 * Bytes from the program.
 *
 * WSAP_VTCORE_FEED_ONLCR: a line feed also returns the cursor to column 0, as
 * a tty with OPOST and ONLCR does. Set it when the bytes did not pass through
 * the tty output processing (for example the fast pipe).
 */
enum wsap_vtcore_feed_flags {
    WSAP_VTCORE_FEED_ONLCR = 1u << 0,
};

/*
 * Feed bytes from the program. Returns false, and changes nothing, when t is
 * NULL, when bytes is NULL and len is not 0, or when flags has a bit that is
 * not in enum wsap_vtcore_feed_flags. A len of 0 is a no-op that returns true.
 */
WSAP_VTCORE_API bool wsap_vtcore_feed(struct wsap_vtcore *t, const char *bytes, size_t len, unsigned flags);

/* Report a focus change. The terminal replies only in focus-event mode. */
WSAP_VTCORE_API void wsap_vtcore_focus(struct wsap_vtcore *t, bool focused);

/* Modes that affect how the application encodes input. */
struct wsap_vtcore_modes {
    unsigned keyboard;                   /* kitty keyboard protocol flags */
    enum wsap_vtcore_mouse mouse;
    enum wsap_vtcore_mouse_encoding mouse_encoding;
    struct wsap_vtcore_vector2i grid;           /* columns and rows */
    bool app_cursor;                     /* DECCKM */
    bool app_keypad;                     /* DECKPAM */
    bool backarrow_bs;                   /* DECBKM */
    bool bracketed_paste;
    bool focus_events;
    bool alternate;                      /* the alternate screen is active */
    bool reverse_video;                  /* DECSCNM */
    bool synchronized;                   /* mode 2026 */
};

struct wsap_vtcore_cursor {
    struct wsap_vtcore_vector2i pos;
    enum wsap_vtcore_cursor_style style;
    bool visible;
    bool blink;
};

WSAP_VTCORE_API struct wsap_vtcore_vector2i wsap_vtcore_size(const struct wsap_vtcore *t);
WSAP_VTCORE_API struct wsap_vtcore_modes wsap_vtcore_modes(const struct wsap_vtcore *t);

/*
 * Encode an input event as the bytes the program expects. Get modes from
 * wsap_vtcore_modes(). Record ev in keyboard with
 * wsap_vtcore_keyboard_update() first. Send the bytes to the program yourself.
 */
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

/* Bytes wsap_vtcore_input_encode_paste() adds to the text at most. */
#define WSAP_VTCORE_PASTE_EXTRA 12

/*
 * Encode pasted text. Drop control characters and bidi controls, so the
 * program sees only what the user saw. Send a line break as CR. Add the
 * bracketed-paste markers when the program asked for them. A buffer of
 * len + WSAP_VTCORE_PASTE_EXTRA bytes is always large enough. Text that is
 * empty after the filter gives WSAP_VTCORE_INPUT_CONSUMED.
 */
WSAP_VTCORE_API struct wsap_vtcore_input_result wsap_vtcore_input_encode_paste(
    const struct wsap_vtcore_modes *modes,
    const char *text, size_t len,
    char *buf, size_t capacity
);
/* The cursor in screen coordinates (not the viewport). */
WSAP_VTCORE_API struct wsap_vtcore_cursor wsap_vtcore_cursor(const struct wsap_vtcore *t);
/* Owned, NUL-terminated text from OSC 7 and OSC 22. NULL when unavailable
 * or allocation fails. The strings stay valid after the terminal changes
 * or is freed. Release them with wsap_vtcore_text_free(). */
WSAP_VTCORE_API char *wsap_vtcore_cwd(const struct wsap_vtcore *t);
WSAP_VTCORE_API char *wsap_vtcore_pointer_shape(const struct wsap_vtcore *t);
/* Release any owned text returned by the library. NULL is safe. */
WSAP_VTCORE_API void wsap_vtcore_text_free(char *text);

/*
 * Viewport. The viewport shows as many rows as the screen. Offset 0 shows
 * the live screen; a larger offset shows that many rows of history above it.
 * The alternate screen has no history, so its offset is always 0.
 */
struct wsap_vtcore_scrollback {
    int offset;     /* rows between the viewport and the live screen */
    int total;      /* history rows at the current width */
};

WSAP_VTCORE_API struct wsap_vtcore_scrollback wsap_vtcore_scrollback(const struct wsap_vtcore *t);
/* Scroll the viewport. A positive count moves back into history. */
WSAP_VTCORE_API void wsap_vtcore_scroll(struct wsap_vtcore *t, int lines);
WSAP_VTCORE_API void wsap_vtcore_scroll_to(struct wsap_vtcore *t, int offset);

/*
 * Selection, in viewport coordinates. A selection starts at one cell and
 * extends to another; the unit widens both ends to words or whole lines.
 * The terminal clears the selection when the text under it changes.
 */
enum wsap_vtcore_select_unit {
    WSAP_VTCORE_SELECT_CELL,
    WSAP_VTCORE_SELECT_WORD,
    WSAP_VTCORE_SELECT_LINE,
};

/* Start a selection at cell. For WSAP_VTCORE_SELECT_CELL, right_half means the
 * pointer is on the right half of the cell, so the selection starts after it.
 * Returns false when memory runs out; the selection is then unchanged. */
WSAP_VTCORE_API bool wsap_vtcore_select_start(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half,
                       enum wsap_vtcore_select_unit unit);
/* Extend the selection from its start to cell. Returns false when memory runs
 * out; the selection is then unchanged. */
WSAP_VTCORE_API bool wsap_vtcore_select_extend(struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell, bool right_half);
WSAP_VTCORE_API void wsap_vtcore_select_clear(struct wsap_vtcore *t);
WSAP_VTCORE_API bool wsap_vtcore_has_selection(const struct wsap_vtcore *t);
/* Owned, NUL-terminated UTF-8 of the selection. NULL when nothing is
 * selected or allocation fails; a selection of blanks may produce an empty
 * string. Capture is atomic with respect to other calls on t. Release the
 * result with wsap_vtcore_text_free(). */
WSAP_VTCORE_API char *wsap_vtcore_selection_text(const struct wsap_vtcore *t);
/* Owned OSC 8 link or detected URL at a viewport cell. NULL when there is no
 * link or allocation fails. Release the result with wsap_vtcore_text_free(). */
WSAP_VTCORE_API char *wsap_vtcore_link_at(const struct wsap_vtcore *t, struct wsap_vtcore_vector2i cell);

/* Cell attributes share the write core's bit layout. Underline styles are
 * mutually exclusive flags, not a shifted enum; use
 * wsap_vtcore_attr_underline(). */
enum wsap_vtcore_attr {
    WSAP_VTCORE_ATTR_BOLD = 1u << 0,
    WSAP_VTCORE_ATTR_ITALIC = 1u << 1,
    WSAP_VTCORE_ATTR_UNDERLINE = 1u << 2,
    WSAP_VTCORE_ATTR_STRIKE = 1u << 3,
    WSAP_VTCORE_ATTR_OVERLINE = 1u << 4,
    WSAP_VTCORE_ATTR_EMOJI = 1u << 5,        /* the text requested emoji presentation */
    WSAP_VTCORE_ATTR_TEXT = 1u << 6,         /* the text requested text presentation */
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
    if (attrs & WSAP_VTCORE_ATTR_DOUBLE_UNDERLINE) return WSAP_VTCORE_UNDERLINE_DOUBLE;
    if (attrs & WSAP_VTCORE_ATTR_CURLY_UNDERLINE) return WSAP_VTCORE_UNDERLINE_CURLY;
    if (attrs & WSAP_VTCORE_ATTR_DOTTED_UNDERLINE) return WSAP_VTCORE_UNDERLINE_DOTTED;
    if (attrs & WSAP_VTCORE_ATTR_DASHED_UNDERLINE) return WSAP_VTCORE_UNDERLINE_DASHED;
    return attrs & WSAP_VTCORE_ATTR_UNDERLINE ? WSAP_VTCORE_UNDERLINE_SINGLE : WSAP_VTCORE_UNDERLINE_NONE;
}

/* text refers to a grapheme cluster in the snapshot; see
 * wsap_vtcore_snapshot_cluster(). */
#define WSAP_VTCORE_TEXT_CLUSTER 0x80000000u

/* One cell as a snapshot shows it. */
struct wsap_vtcore_cell {
    uint32_t text;      /* a code point, WSAP_VTCORE_TEXT_CLUSTER | n, or 0 for an empty cell */
    wsap_vtcore_color fg, bg;
    uint16_t attrs;     /* enum wsap_vtcore_attr flags; at most one underline flag */
    uint8_t width;      /* 1; 2 for a wide glyph; 0 for the right half of a wide glyph */
    uint8_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(struct wsap_vtcore_cell) == 16, "four cells fill one 64-byte line");
#else
_Static_assert(sizeof(struct wsap_vtcore_cell) == 16, "four cells fill one 64-byte line");
#endif

/*
 * Snapshot: a copy of the viewport for a renderer. It holds no pointer into
 * the terminal, so it stays valid while the terminal changes. One terminal
 * may have several snapshots, for example a window and a tab preview.
 */
WSAP_VTCORE_API struct wsap_vtcore_snapshot *wsap_vtcore_snapshot_new(void);
WSAP_VTCORE_API void wsap_vtcore_snapshot_free(struct wsap_vtcore_snapshot *s);

/* Copy the viewport of t into s. Keep rows with unchanged content keys.
 * Update the history view index and refine counts for visible wide lines.
 * Return false when memory runs out. The snapshot then keeps its previous
 * content. */
WSAP_VTCORE_API bool wsap_vtcore_snapshot_update(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t);

/* Palette entries of struct wsap_vtcore_view.palette after the 256 indexed
 * colors. */
enum {
    WSAP_VTCORE_PALETTE_FG = 256,
    WSAP_VTCORE_PALETTE_BG = 257,
    WSAP_VTCORE_PALETTE_CURSOR = 258,
    WSAP_VTCORE_PALETTE_SIZE = 259,
};

struct wsap_vtcore_view {
    struct wsap_vtcore_vector2i size;
    struct wsap_vtcore_cursor cursor;  /* viewport coordinates; not visible when scrolled away */
    int scroll_offset;
    /* Rows the content moved up since the previous update, when that movement
     * explains the change; otherwise 0. */
    int scrolled;
    bool reverse_video;
    /* Changes when any part of the view changes. */
    uint64_t version;
    /* Changes when a palette entry changes. */
    uint64_t palette_version;
    const uint32_t *palette;    /* WSAP_VTCORE_PALETTE_SIZE ARGB entries */
};

/* The view of s, NULL for a NULL s. The view and view->palette keep their
 * address until wsap_vtcore_snapshot_free(); their contents change on each
 * update. */
WSAP_VTCORE_API const struct wsap_vtcore_view *wsap_vtcore_snapshot_view(const struct wsap_vtcore_snapshot *s);
/* The cells of viewport row y: view->size.x entries. NULL when y is outside
 * the viewport or s is NULL. An update can move the cells, so the pointer is
 * valid only until the next wsap_vtcore_snapshot_update() or
 * wsap_vtcore_snapshot_free() of s. */
WSAP_VTCORE_API const struct wsap_vtcore_cell *wsap_vtcore_snapshot_row(const struct wsap_vtcore_snapshot *s, int y);
/* An identity for the content of row y. Two rows with the same key hold the
 * same cells, also when the content moved to another row. 0 when y is outside
 * the viewport or s is NULL. */
WSAP_VTCORE_API uint64_t wsap_vtcore_snapshot_row_key(const struct wsap_vtcore_snapshot *s, int y);

/* Number of rows in history above the screen. 0 if alternate screen or no history. */
WSAP_VTCORE_API int wsap_vtcore_history_rows(const struct wsap_vtcore *t);
/* Owned, NUL-terminated UTF-8 of up to max_rows history rows before the
 * current viewport. Join soft-wrapped rows and end closed lines with a
 * newline. Does not change the selection. No history or max_rows <= 0 gives
 * an empty string; NULL means allocation failed or t is NULL. Release the
 * result with wsap_vtcore_text_free(). */
WSAP_VTCORE_API char *wsap_vtcore_history_text(struct wsap_vtcore *t, int max_rows);
/* The selected columns of viewport row y, end exclusive. An empty span when y
 * is outside the viewport or s is NULL. */
WSAP_VTCORE_API struct wsap_vtcore_span wsap_vtcore_snapshot_row_selection(const struct wsap_vtcore_snapshot *s, int y);

/* The code points of a cluster cell. Returns the number of code points and
 * writes at most cap of them. Writes nothing when cps is NULL or cap is 0.
 * Returns 0 when s is NULL. */
WSAP_VTCORE_API int wsap_vtcore_snapshot_cluster(const struct wsap_vtcore_snapshot *s, uint32_t text, uint32_t *cps, int cap);

/* The ARGB value of a color in a view. */
static inline uint32_t wsap_vtcore_view_color(const struct wsap_vtcore_view *v, wsap_vtcore_color c)
{
    if (!wsap_vtcore_color_is_slot(c))
        return c;
    return v->palette[c & WSAP_VTCORE_COLOR_SLOT_INDEX];
}

#ifdef __cplusplus
}
#endif

#endif
