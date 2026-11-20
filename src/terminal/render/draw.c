#include "render/core.h"
#include "render/gpu/gpu.h"
#include "terminal/render/term_frame.h"
#include "terminal/render/view.h"

struct render_result render_draw(struct renderer *r, struct framebuffer *fb,
                                 const struct render_frame *frame)
{
    struct render_grid_view grid;
    const struct render_grid_view *view =
        frame && render_term_grid_view(frame->grid.frame, &grid) ? &grid : NULL;
    return render_draw_view(r, fb, frame, view);
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
    return renderer_cursor_band(r, font, &grid, tf->cursor_y, span_begin, span_cols,
                                out, max_px, out_w, out_h);
}

enum render_gpu_status render_gpu_draw(struct render_gpu *g, struct render_surface *s,
                                       struct framebuffer *fb, const struct render_frame *frame)
{
    struct render_grid_view grid;
    const struct render_grid_view *view =
        frame && render_term_grid_view(frame->grid.frame, &grid) ? &grid : NULL;
    return render_gpu_draw_view(g, s, fb, frame, view);
}
