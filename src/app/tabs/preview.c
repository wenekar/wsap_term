#include <stdlib.h>

#include "app/tabs/internal.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/widgets/tab_preview.h"

static bool paint_thumbnail(struct app_tabs *tabs, int index, int width)
{
    struct window *w = tabs->window;
    if (!w || !window_surface(w) || index < 0 || index >= tabs->count)
        return false;
    struct terminal *content = tabs->tabs[index].content;
    struct session *session = terminal_session(content);
    if (!content || !session || !session->term)
        return false;
    int available = window_size(w).x;
    if (width > available)
        width = available;
    return tabs->spec.preview_capture && tabs->spec.preview_capture(
        tabs->spec.preview_ud, &tabs->preview_state, terminal_font(content),
        session->term, width, &tabs->preview_image);
}

static bool capture(void *userdata, int index, int width, bool dragging,
                    const char **title, const struct ui_image **image)
{
    (void)dragging;
    struct app_tabs *tabs = userdata;
    if (!paint_thumbnail(tabs, index, width))
        return false;
    *title = terminal_title(tabs->tabs[index].content);
    *image = &tabs->preview_image;
    return true;
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
    return paint_thumbnail(tabs, index,
        ui_tab_preview_image_width(&tabs->preview, dragging));
}

void app_tabs_preview_hover(struct app_tabs *tabs, int index)
{
    struct rect2i anchor = {0};
    if (index == tabs->active || index < 0 || index >= tabs->count ||
        !app_tab_is_live(&tabs->tabs[index]) ||
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
