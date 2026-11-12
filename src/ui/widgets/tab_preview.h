#ifndef WSAP_UI_WIDGETS_TAB_PREVIEW_H
#define WSAP_UI_WIDGETS_TAB_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_tab_preview_source {
    bool (*capture)(void *userdata, int index, int width, bool dragging,
                    const char **title, const struct ui_image **image);
    void *userdata;
};

struct ui_tab_preview {
    struct ui_widget widget;
    struct {
        struct ui_tab_preview_source source;
        char *title;
        /* The thumbnail of the tab, held by value: a new capture compares
         * against it. No pixels means no image. */
        struct ui_image image;
        struct rect2i anchor;
        uint64_t show_at;
        uint64_t refresh_at;
        int index;
        int pending;
        bool dragging;
        bool shown;
        bool suppressed;
    } impl;
};

/* Initializes tab preview popup `p`. */
void ui_tab_preview_init(struct ui_tab_preview *p);
/* Sets the tab capture source callback for `p`. */
void ui_tab_preview_set_source(struct ui_tab_preview *p,
                               const struct ui_tab_preview_source *source);
/* Schedules preview display for the tab at `index` anchored at `anchor`. */
void ui_tab_preview_hover(struct ui_tab_preview *p, int index,
                          struct rect2i anchor);
/* Suppresses preview display until the next hover event. */
void ui_tab_preview_suppress(struct ui_tab_preview *p);
/* Updates the drag preview position to coordinates `at` for tab `index`. */
void ui_tab_preview_follow(struct ui_tab_preview *p, int index,
                           struct vector2i at);
/* Hides and resets the tab preview popup. */
void ui_tab_preview_hide(struct ui_tab_preview *p);
/* Returns true when the tab preview popup is visible. */
bool ui_tab_preview_is_visible(const struct ui_tab_preview *p);
/* Returns the thumbnail image width in pixels for dragging or resting mode. */
int ui_tab_preview_image_width(const struct ui_tab_preview *p, bool dragging);

/* Returns the base widget pointer of `p`, or NULL if `p` is NULL. */
static inline struct ui_widget *ui_tab_preview_widget(struct ui_tab_preview *p)
{
    return p ? &p->widget : NULL;
}

#endif
