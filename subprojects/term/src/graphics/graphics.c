#include "graphics.h"
#include "core.h"
#include "screen.h"

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
    cmd->cursor = (struct wsap_vtcore_vector2i){t->cursor_x, t->cursor_y};
    cmd->alternate = t->alternate;
    cmd->cell_pixels = t->cell_pixels;
    cmd->grid = (struct wsap_vtcore_vector2i){t->cols, t->rows};
    struct wsap_vtcore_vector2i extent = {0};
    enum wsap_vtcore_graphics_result result = t->graphics_receive(t->graphics_ud, cmd, &extent);
    if (result == WSAP_VTCORE_GRAPHICS_OK && cmd->action == WSAP_VTCORE_GRAPHICS_SIXEL && extent.y > 0) {
        int visible = t->rows - t->cursor_y;
        if (t->cursor_y <= t->scroll_bottom) visible = t->scroll_bottom - t->cursor_y + 1;
        /* The decoder made space before placement; do not scroll the first
         * band off the screen when a bitmap fills the whole region. */
        if (t->cell_pixels.y > 0 && !lr_margins_partial(t)) visible--;
        int advance = extent.y < visible ? extent.y : visible;
        if (advance > 0) sc_linefeed(t, (size_t)advance);
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
    struct wsap_vtcore_graphics reset = {.action = WSAP_VTCORE_GRAPHICS_RESET};
    wsap_vtcore_graphics_emit(t, &reset);
}
