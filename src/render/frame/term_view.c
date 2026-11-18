#include "render/frame/term_view.h"

#include "render/frame/walk.h"
#include "render/surface.h"
#include "render/term_frame.h"

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

static uint64_t term_row_key(const void *source, int row)
{
    return render_surface_row_key(source, row);
}

static bool term_reserve_walk(int cols)
{
    return render_walk_thread_reserve(cols);
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
