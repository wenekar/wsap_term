#include <limits.h>
#include <stdlib.h>

#include "app/tabs/internal.h"
#include "font/font.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/surface.h"
#include "ui/ui_util.h"
#include "ui/widgets/tab_preview.h"

enum { THUMB_MIN_DP = 24 };

static int fit_width(struct window *w, const struct font_instance *font,
                     const struct wsap_vtcore *term, int width, int max_height)
{
    const struct ui_surface *surface = window_ui_surface(w);
    float dpi = (!window_surface(w) && surface)
        ? ui_surface_scale(surface).dpi_scale
        : window_ui_scale(w).dpi_scale;
    int min_px = ui_px(dpi, THUMB_MIN_DP);
    if (min_px < 1)
        min_px = 1;
    int available = window_size(w).x;
    if (available <= 0 && !window_surface(w) && surface)
        available = ui_surface_viewport(surface).x;
    if (width > available)
        width = available;
    if (width < min_px || max_height < min_px)
        return 0;
    struct wsap_vtcore_vector2i cells = wsap_vtcore_size(term);
    const struct font_metrics *metrics = font_instance_metrics(font);
    if (!metrics || cells.x <= 0 || cells.y <= 0 ||
        metrics->cell_w <= 0 || metrics->cell_h <= 0)
        return 0;
    int64_t full_x = (int64_t)cells.x * metrics->cell_w;
    int64_t full_y = (int64_t)cells.y * metrics->cell_h;
    if (full_y > max_height) {
        int64_t cap_w = (int64_t)max_height * full_x / full_y;
        if (cap_w < width)
            width = (int)cap_w;
    }
    int64_t scaled_w = full_x < width ? full_x : width;
    int64_t scaled_h = (full_y * scaled_w) / full_x;
    /* Drop thumbnails smaller than min_px in either axis. Tight budgets
     * then show a title-only card instead of a sliver. */
    if (width < min_px || scaled_w < min_px || scaled_h < min_px)
        return 0;
    return width;
}

static bool paint_thumbnail(struct app_tabs *tabs, int index, int width,
                            int max_height, const char **title,
                            const struct ui_image **image)
{
    struct window *w = tabs->window;
    if (!w || window_closed(w) || index < 0 || index >= tabs->count)
        return false;
    struct terminal *content = app_tabs_content(tabs, index);
    struct session *session = terminal_session(content);
    struct font_instance *font = terminal_font(content);
    if (!content || !session || !session->term || !font ||
        !tabs->spec.preview_capture)
        return false;
    if (title)
        *title = terminal_title(content);
    int fit_w = fit_width(w, font, session->term, width, max_height);
    if (fit_w <= 0) {
        if (image)
            *image = NULL;
        return true;
    }
    if (!tabs->spec.preview_capture(tabs->spec.preview_ud, &tabs->preview_state,
                                    font, session->term, fit_w,
                                    &tabs->preview_image))
        return false;
    if (image)
        *image = &tabs->preview_image;
    return true;
}

static bool capture(void *userdata, int index, int width, int max_height,
                    bool dragging, const char **title,
                    const struct ui_image **image)
{
    (void)dragging;
    return paint_thumbnail(userdata, index, width, max_height, title, image);
}

void app_tabs_preview_init(struct app_tabs *tabs)
{
    ui_tab_preview_set_source(&tabs->preview,
                              &(struct ui_tab_preview_source){
                                  .capture = capture, .userdata = tabs,
                              });
}

bool app_tabs_preview_capture(struct app_tabs *tabs, int index, bool dragging)
{
    const struct ui_image *image = NULL;
    return paint_thumbnail(tabs, index,
                           ui_tab_preview_image_width(&tabs->preview, dragging),
                           INT_MAX, NULL, &image) && image != NULL;
}

void app_tabs_preview_hover(struct app_tabs *tabs, int index)
{
    struct rect2i anchor = {0};
    if (index == tabs->active || index < 0 || index >= tabs->count ||
        !app_tab_is_live(app_tabs_entry(tabs, index)) ||
        !ui_tabbar_tab_rect(&tabs->tabbar, index, &anchor))
        index = -1;
    ui_tab_preview_hover(&tabs->preview, index, anchor);
}

void app_tabs_preview_drag(struct app_tabs *tabs, const struct ui_action *action)
{
    if (action->id == UI_TAB_DRAG_DETACH)
        ui_tab_preview_follow(&tabs->preview, action->index, action->pos);
    else
        ui_tab_preview_suppress(&tabs->preview);
}
