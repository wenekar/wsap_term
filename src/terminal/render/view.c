#include "terminal/render/view.h"

#include "terminal/render/term_frame.h"
#include "terminal/render/walk.h"
#include "util/hash.h"

/* Tags that keep the selection part and the cursor part of a row key apart. */
#define ROW_KEY_SELECTION UINT64_C(0x51E5E1EC7105E1EC)
#define ROW_KEY_CURSOR UINT64_C(0xC0125012C0125012)

static void term_walk(struct font_instance *font, const void *source, int row,
                      int c0, int c1, unsigned flags, const struct render_walk_sink *sink)
{
    render_row_walk(font, source, row, c0, c1, flags, sink);
}

static enum render_line_size term_line_size(const void *source, int row)
{
    switch (terminal_frame_row_size(source, row)) {
    case WSAP_VTCORE_LINE_DOUBLE_WIDTH:
        return RENDER_LINE_DOUBLE_WIDTH;
    case WSAP_VTCORE_LINE_DOUBLE_TOP:
        return RENDER_LINE_DOUBLE_TOP;
    case WSAP_VTCORE_LINE_DOUBLE_BOTTOM:
        return RENDER_LINE_DOUBLE_BOTTOM;
    case WSAP_VTCORE_LINE_SINGLE:
    default:
        return RENDER_LINE_SINGLE;
    }
}

/*
 * The key of grid row `row` as the canvas shows it: a mix of the content
 * key, the selection span of the row, and the cursor x and style when the
 * cursor is on the row. It is never 0 for a row that the planner can keep
 * or move.
 */
uint64_t render_term_row_key(const struct terminal_frame *f, int row)
{
    uint64_t tk = terminal_frame_row_key(f, row);
    /* An unknown row keeps key 0. The planner never matches or keeps it. */
    if (!tk)
        return 0;
    uint64_t h = hash64_finish(tk ^ HASH64_BASIS);
    if (f && f->has_selection) {
        struct wsap_vtcore_span sel = terminal_frame_row_selection(f, row);
        if (sel.end > sel.begin)
            h = hash64_finish(h ^ ROW_KEY_SELECTION ^
                              ((uint64_t)(uint32_t)sel.begin << 32 | (uint32_t)sel.end));
    }
    if (f && f->cursor_y >= 0 && f->cursor_y == row)
        h = hash64_finish(h ^ ROW_KEY_CURSOR ^
                          ((uint64_t)(uint32_t)f->cursor_x << 32 | (uint32_t)f->cursor_style));
    return h ? h : 1;
}

static bool term_reserve_walk(int cols)
{
    return render_walk_thread_reserve(cols);
}

static uint64_t term_row_key(const void *source, int row)
{
    return render_term_row_key(source, row);
}

static const struct render_grid_ops term_grid_ops = {
    .walk = term_walk,
    .line_size = term_line_size,
    .row_key = term_row_key,
    .reserve_walk = term_reserve_walk,
};

bool render_term_grid_view(const struct terminal_frame *tf, struct render_grid_view *out)
{
    if (!tf || !terminal_frame_valid(tf) || !out)
        return false;
    *out = (struct render_grid_view){
        .ops = &term_grid_ops,
        .source = tf,
        .cells = { .x = tf->cols, .y = tf->rows },
        .palette_gen = tf->palette_gen,
        .default_fg = tf->default_fg,
        .default_bg = tf->default_bg,
        .cursor_color = tf->cursor_color,
        .selection_fg = terminal_frame_selection_fg(tf),
        .selection_bg = terminal_frame_selection_bg(tf),
        .reverse_video = tf->reverse_video,
        .blink_on = tf->blink_on,
    };
    return true;
}
