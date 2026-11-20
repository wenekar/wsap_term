#include <limits.h>
#include <stdlib.h>

#include "app/tabs/internal.h"
#include "ui/widgets/tab_preview.h"

static bool paint_thumbnail(struct app_tabs *tabs, int index, int width,
                            int max_height, const char **title,
                            const struct ui_image **image)
{
    struct app_tab *entry = app_tabs_entry(tabs, index);
    if (!entry || !entry->content)
        return false;
    if (title)
        *title = app_tab_title(entry);
    tabs->preview_image = (struct ui_image){0};
    if (!entry->ops->thumbnail ||
        !entry->ops->thumbnail(entry->content,
                               (struct vector2i){ .x = width,
                                                  .y = max_height },
                               &tabs->preview_state, &tabs->preview_image))
        return false;
    if (image)
        *image = tabs->preview_image.pixels ? &tabs->preview_image : NULL;
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

bool app_tabs_preview_capture(struct app_tabs *tabs, int index)
{
    const struct ui_image *image = NULL;
    return paint_thumbnail(tabs, index,
                           ui_tab_preview_image_width(&tabs->preview, true),
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
