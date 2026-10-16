/* Test-only entry points for the Kitty screen wrapper in screen.py.
 * Python calls only these functions, so it needs no struct layouts other
 * than the public struct wsap_vtcore_cell. The kitty_screen_* accessors read
 * properties that wsap_vtcore.h does not expose. They compile against the current
 * terminal layout rather than duplicating byte offsets in Python. */
#include "internal.h"
#include "screen.h"

typedef void (*kitty_reply_fn)(void *ud, const char *bytes, size_t len);

struct wsap_vtcore *kitty_term_new(int cols, int rows, kitty_reply_fn reply, void *ud)
{
    return wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = cols, .y = rows },
        .callbacks = { .ud = ud, .reply = reply },
    });
}

/* One snapshot serves every cell read. */
static struct wsap_vtcore_snapshot *snap;

void kitty_term_free(struct wsap_vtcore *t)
{
    /* A new terminal can get the address of the freed one. The snapshot
     * would then keep its rows, so it goes away too. */
    wsap_vtcore_snapshot_free(snap);
    snap = NULL;
    wsap_vtcore_free(t);
}

void kitty_term_write(struct wsap_vtcore *t, const char *bytes, size_t len)
{
    wsap_vtcore_feed(t, bytes, len, 0);
}

bool kitty_term_resize(struct wsap_vtcore *t, int cols, int rows)
{
    return wsap_vtcore_resize(t, (struct wsap_vtcore_vector2i){ .x = cols, .y = rows });
}

int kitty_term_cols(struct wsap_vtcore *t) { return wsap_vtcore_size(t).x; }
int kitty_term_rows(struct wsap_vtcore *t) { return wsap_vtcore_size(t).y; }
int kitty_term_cursor_x(struct wsap_vtcore *t) { return wsap_vtcore_cursor(t).pos.x; }
int kitty_term_cursor_y(struct wsap_vtcore *t) { return wsap_vtcore_cursor(t).pos.y; }
int kitty_term_cursor_style(struct wsap_vtcore *t) { return (int)wsap_vtcore_cursor(t).style; }
bool kitty_term_cursor_blink(struct wsap_vtcore *t) { return wsap_vtcore_cursor(t).blink; }
bool kitty_term_application_cursor(struct wsap_vtcore *t) { return wsap_vtcore_modes(t).app_cursor; }

/* Update the snapshot from the live screen, as a renderer reads it.
 * Returns NULL when the cell is out of range or memory runs out. */
static struct wsap_vtcore_snapshot *cell_snapshot(struct wsap_vtcore *t, int col, int row)
{
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t);
    if (col < 0 || col >= size.x || row < 0 || row >= size.y)
        return NULL;
    if (!snap)
        snap = wsap_vtcore_snapshot_new();
    wsap_vtcore_scroll_to(t, 0);
    if (!snap || !wsap_vtcore_snapshot_update(snap, t))
        return NULL;
    return snap;
}

bool kitty_term_cell(struct wsap_vtcore *t, int col, int row, struct wsap_vtcore_cell *out)
{
    struct wsap_vtcore_snapshot *s = cell_snapshot(t, col, row);
    if (!s)
        return false;
    *out = wsap_vtcore_snapshot_row(s, row)[col];
    return true;
}

/* The code points of a cell's text: one, or a whole grapheme cluster.
 * Returns the number of code points and writes at most cap of them. */
int kitty_term_cell_text(struct wsap_vtcore *t, int col, int row, uint32_t *cps, int cap)
{
    struct wsap_vtcore_snapshot *s = cell_snapshot(t, col, row);
    if (!s)
        return 0;
    return wsap_vtcore_snapshot_cluster(s, wsap_vtcore_snapshot_row(s, row)[col].text, cps, cap);
}

#define PROPERTY(type, name) \
    type kitty_screen_##name(struct wsap_vtcore *t) { return t->name; }

PROPERTY(int, scroll_top)
PROPERTY(int, scroll_bottom)
PROPERTY(uint32_t, cur_fg)
PROPERTY(uint32_t, cur_bg)
PROPERTY(bool, bold)
PROPERTY(bool, faint)
PROPERTY(bool, italic)
PROPERTY(bool, strike)
PROPERTY(bool, inverse)
PROPERTY(uint8_t, underline)

int kitty_screen_row_length(struct wsap_vtcore *t, int row)
{
    if (!t || row < 0 || row >= t->rows) return 0;
    const struct srow *r = wsap_vtcore_row(t, row);
    return r ? (int)r->len : 0;
}

/* A tab stores a marker cell that a snapshot shows as a blank. Kitty keeps
 * the tab in its line text, so the wrapper asks for the marker. */
bool kitty_screen_cell_is_tab(struct wsap_vtcore *t, int col, int row)
{
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows)
        return false;
    wsap_vtcore_scroll_to(t, 0);
    const struct cell *cells = wsap_vtcore_screen_view_row(t, row);
    return cells && cells[col].c == '\t';
}
