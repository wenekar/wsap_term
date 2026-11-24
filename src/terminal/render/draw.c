#include "terminal/render/term_frame.h"
#include "terminal/render/term_render.h"
#include "terminal/render/view.h"

static struct renderer_frame resolve_frame(const struct render_frame *frame,
                                           struct render_grid_view *grid)
{
    bool has_grid = render_term_grid_view(frame->grid.frame, grid);
    if (has_grid) {
        grid->font = frame->grid.font;
        grid->rect = frame->grid.rect;
    }
    return (struct renderer_frame){
        .grid = has_grid ? grid : NULL,
        .graphics = frame->graphics,
        .ui = frame->ui,
        .ui_font = frame->ui_font,
        .ui_damage = frame->ui_damage,
        .base = frame->base,
        .repaint = frame->repaint,
        .cpu_only = frame->cpu_only,
    };
}

struct render_result render_draw(struct renderer *r, struct framebuffer *fb,
                                 const struct render_frame *frame)
{
    if (!frame)
        return (struct render_result){0};
    struct render_grid_view grid;
    struct renderer_frame resolved = resolve_frame(frame, &grid);
    return renderer_draw(r, fb, &resolved);
}

bool render_cursor_cell(struct renderer *r, struct font_instance *font,
                        const struct terminal_frame *tf,
                        int span_begin, int span_cols,
                        uint32_t *out, size_t max_px,
                        int *out_w, int *out_h)
{
    if (!tf || tf->cursor_y < 0 || tf->cursor_y >= tf->rows ||
        terminal_frame_row_size(tf, tf->cursor_y) != WSAP_VTCORE_LINE_SINGLE)
        return false;
    struct render_grid_view grid;
    if (!render_term_grid_view(tf, &grid))
        return false;
    grid.font = font;
    return renderer_cursor_band(r, &grid, tf->cursor_y, span_begin, span_cols,
                                out, max_px, out_w, out_h);
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    if (!frame)
        return RENDER_GPU_UNUSED;
    struct render_grid_view grid;
    struct renderer_frame resolved = resolve_frame(frame, &grid);
    return render_gpu_draw_frame(g, s, fb, &resolved);
}
