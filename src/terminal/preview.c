#include "terminal/internal.h"

static bool paint_thumbnail(struct window *w, int index, int width)
{
    struct terminal *tw = terminal_of(w);
    if (!window_surface(w) || index < 0 || index >= tw->tab_count ||
        !tw->tabs[index]->session.term)
        return false;
    int available = window_size(w).x;
    if (width > available)
        width = available;
    return tw->preview_capture && tw->preview_capture(
        tw->preview_ud, &tw->preview_state, tw->tabs[index]->font,
        tw->tabs[index]->session.term, width, &tw->preview_image);
}

bool terminal_tab_preview_capture(struct window *w, int index, bool dragging)
{
    struct terminal *tw = terminal_of(w);
    return paint_thumbnail(w, index,
                           ui_tab_preview_image_width(&tw->tab_preview, dragging));
}

static bool capture(void *userdata, int index, int width, bool dragging,
                    const char **title, const struct ui_image **image)
{
    (void)dragging;
    struct window *w = userdata;
    struct terminal *tw = terminal_of(w);
    if (!paint_thumbnail(w, index, width))
        return false;
    *title = session_title(&tw->tabs[index]->session);
    *image = &tw->preview_image;
    return true;
}

void terminal_tab_preview_init(struct window *w)
{
    ui_tab_preview_set_source(&terminal_of(w)->tab_preview,
                              &(struct ui_tab_preview_source){
                                  .capture = capture, .userdata = w,
                              });
}
