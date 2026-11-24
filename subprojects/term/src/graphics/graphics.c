#include "graphics.h"
#include "core.h"
#include "screen.h"
#include <stdlib.h>
#include <string.h>

#define GRAPHICS_HISTORY_MAX (64 * 1024)

struct graphics_anchor {
    uint64_t line, cell, uid;
};

struct wsap_vtcore_graphics_history {
    struct graphics_anchor *anchors;
    size_t first, end, cap;
};

void wsap_vtcore_graphics_track_rows(struct wsap_vtcore *t, int first, int count)
{
    if (!t || count <= 0)
        return;
    wsap_vtcore_lock(t);
    int64_t end = (int64_t)first + count;
    for (int y = first < 0 ? 0 : first; y < t->rows && y < end; y++)
        wsap_vtcore_row(t, y)->uid |= GRAPHICS_ROW_BIT;
    wsap_vtcore_unlock(t);
}

static size_t anchor_lower(const struct wsap_vtcore_graphics_history *h, uint64_t line, uint64_t cell)
{
    size_t lo = h->first, hi = h->end;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const struct graphics_anchor *a = &h->anchors[mid];
        if (a->line < line || (a->line == line && a->cell < cell)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static size_t first_anchor(const struct wsap_vtcore *t)
{
    uint64_t line = hist_first_id(&t->hist);
    uint64_t cell = line == t->hist.base_cell_id ? t->hist.base_cell : 0;
    return anchor_lower(t->graphics_history, line, cell);
}

bool wsap_vtcore_graphics_has_history(const struct wsap_vtcore *t)
{
    if (!t)
        return false;
    struct wsap_vtcore *locked = (struct wsap_vtcore *)t;
    wsap_vtcore_lock(locked);
    bool out = !t->alternate && t->hist.lines && t->graphics_history &&
               first_anchor(t) < t->graphics_history->end;
    wsap_vtcore_unlock(locked);
    return out;
}

void wsap_vtcore_graphics_history_add(struct wsap_vtcore *t, uint64_t line, uint64_t cell, uint64_t uid)
{
    if (line < hist_first_id(&t->hist) || line >= hist_end_id(&t->hist) ||
        (line == t->hist.base_cell_id && cell < t->hist.base_cell))
        return;
    struct wsap_vtcore_graphics_history *h = t->graphics_history;
    if (!h) {
        h = calloc(1, sizeof(*h));
        if (!h) return;
        t->graphics_history = h;
    }
    h->first = first_anchor(t);
    if (h->end && h->first == h->end) h->first = h->end = 0;
    if (h->end > h->first) {
        const struct graphics_anchor *prev = &h->anchors[h->end - 1];
        if (prev->line == line && prev->cell == cell) return;
    }
    if (h->end == h->cap) {
        if (h->cap == GRAPHICS_HISTORY_MAX && !h->first) return;
        if (h->first > h->cap / 2 || h->cap == GRAPHICS_HISTORY_MAX) {
            memmove(h->anchors, h->anchors + h->first, (h->end - h->first) * sizeof(*h->anchors));
            h->end -= h->first;
            h->first = 0;
        } else {
            size_t cap = h->cap ? h->cap * 2 : 64;
            struct graphics_anchor *next = realloc(h->anchors, cap * sizeof(*next));
            if (!next) return;
            h->anchors = next;
            h->cap = cap;
        }
    }
    h->anchors[h->end++] = (struct graphics_anchor){line, cell, uid};
}

uint64_t wsap_vtcore_graphics_history_row(const struct wsap_vtcore *t, uint64_t line,
                                          int segment, int cols)
{
    const struct wsap_vtcore_graphics_history *h = t->graphics_history;
    if (!h || h->first == h->end)
        return 0;
    size_t at = anchor_lower(h, line, 0);
    if (at == h->end || h->anchors[at].line != line)
        return 0;
    uint64_t cell = wsap_vtcore_hist_segment_cell(&t->hist, line, segment, cols);
    at = anchor_lower(h, line, cell);
    return at < h->end && h->anchors[at].line == line && h->anchors[at].cell == cell
        ? h->anchors[at].uid : 0;
}

void wsap_vtcore_graphics_history_cut(struct wsap_vtcore *t, uint64_t line, uint64_t cell)
{
    struct wsap_vtcore_graphics_history *h = t->graphics_history;
    if (h) h->end = anchor_lower(h, line, cell);
}

void wsap_vtcore_graphics_history_free(struct wsap_vtcore *t)
{
    if (t->graphics_history) {
        free(t->graphics_history->anchors);
        free(t->graphics_history);
        t->graphics_history = NULL;
    }
}

void wsap_vtcore_set_graphics(struct wsap_vtcore *t, wsap_vtcore_graphics_fn receive, void *ud)
{
    if (!t)
        return;
    wsap_vtcore_lock(t);
    wsap_vtcore_kitty_abort(t);
    if (t->state == ST_DCS || t->state == ST_DCS_ESC)
        t->dcs_graphics_disabled = true;
    if (t->state == ST_APC || t->state == ST_APC_ESC || t->state == ST_APC_START)
        t->str_truncated = true;
    t->graphics_receive = receive;
    t->graphics_ud = ud;
    wsap_vtcore_unlock(t);
}

enum wsap_vtcore_graphics_result wsap_vtcore_graphics_emit(struct wsap_vtcore *t,
                                                         struct wsap_vtcore_graphics *cmd)
{
    if (!t->graphics_receive)
        return WSAP_VTCORE_GRAPHICS_UNSUPPORTED;
    bool fixed = cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL && t->sixel_fixed;
    cmd->cursor = fixed ? (struct wsap_vtcore_vector2i){0} :
                         (struct wsap_vtcore_vector2i){t->cursor_x, t->cursor_y};
    cmd->alternate = t->alternate;
    cmd->cell_pixels = t->cell_pixels;
    cmd->grid = (struct wsap_vtcore_vector2i){t->cols, t->rows};
    struct wsap_vtcore_graphics_reply reply = {0};
    enum wsap_vtcore_graphics_result result = t->graphics_receive(t->graphics_ud, cmd, &reply);
    struct wsap_vtcore_vector2i extent = reply.extent;
    if (reply.image_id) cmd->image_id = reply.image_id;
    if (result == WSAP_VTCORE_GRAPHICS_OK && cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL &&
        !fixed && extent.y > 0) {
        int visible = t->rows - t->cursor_y;
        if (t->cursor_y <= t->scroll_bottom) visible = t->scroll_bottom - t->cursor_y + 1;
        /* The decoder made space before placement; do not scroll the first
         * band off the screen when a bitmap fills the whole region. */
        if (t->cell_pixels.y > 0 && !lr_margins_partial(t)) visible--;
        int height = extent.y - t->sixel_scroll_right;
        int advance = height < visible ? height : visible;
        if (advance > 0) sc_linefeed(t, (size_t)advance);
        if (t->sixel_scroll_right && extent.x > 0) {
            int right = t->cols - 1 - t->cursor_x;
            sc_move(t, t->cursor_x + (extent.x < right ? extent.x : right), t->cursor_y);
        }
    }
    if (result == WSAP_VTCORE_GRAPHICS_OK && !cmd->keep_cursor &&
        (cmd->action == WSAP_VTCORE_GRAPHICS_PLACE ||
         cmd->action == WSAP_VTCORE_GRAPHICS_TRANSMIT_AND_PLACE) &&
        extent.x >= 0 && extent.y >= 0 && (extent.x || extent.y)) {
        int right = t->cols - 1 - t->cursor_x;
        int down = t->rows - 1 - t->cursor_y;
        sc_move(t, t->cursor_x + (extent.x < right ? extent.x : right),
                t->cursor_y + (extent.y < down ? extent.y : down));
    }
    return result;
}

void wsap_vtcore_graphics_erase(struct wsap_vtcore *t, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    struct wsap_vtcore_graphics erase = {
        .action = WSAP_VTCORE_GRAPHICS_ERASE,
        .source_pos = {x, y}, .source_size = {width, height},
    };
    wsap_vtcore_graphics_emit(t, &erase);
}

void wsap_vtcore_graphics_discard_screen(struct wsap_vtcore *t)
{
    wsap_vtcore_kitty_abort(t);
    struct wsap_vtcore_graphics discard = {.action = WSAP_VTCORE_GRAPHICS_DISCARD_SCREEN};
    wsap_vtcore_graphics_emit(t, &discard);
}

void wsap_vtcore_graphics_reset(struct wsap_vtcore *t)
{
    wsap_vtcore_kitty_abort(t);
    wsap_vtcore_graphics_history_free(t);
    struct wsap_vtcore_graphics reset = {.action = WSAP_VTCORE_GRAPHICS_RESET};
    wsap_vtcore_graphics_emit(t, &reset);
}
