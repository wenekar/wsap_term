#ifndef WSAP_UI_WIDGETS_TAB_PREVIEW_H
#define WSAP_UI_WIDGETS_TAB_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"

/* Where the card gets what it shows. capture fills in the title and a
 * picture of tab index, at most width pixels wide. The title and image
 * must stay valid until the next capture. The source callback and userdata
 * are borrowed until the source is replaced or the preview is destroyed.
 * False means there is nothing to show. */
struct ui_tab_preview_source {
    bool (*capture)(void *userdata, int index, int width, bool dragging,
                    const char **title, const struct ui_image **image);
    void *userdata;
};

/*
 * A hover card: a tab's title above a picture of its contents. Add it as
 * an overlay child of a parent without its own layout. It takes no
 * pointer input.
 *
 * Hovering a tab shows the card below it after a short delay; while it
 * shows, it moves to the next hovered tab at once and refreshes its
 * picture. A dragged tab's card follows the pointer.
 *
 * CSS: ".tab-preview" (and ".tab-preview.dragging") styles the card;
 * its width sets the picture width. ".tab-preview-title" styles the title.
 */
struct ui_tab_preview {
    struct ui_widget widget;
    struct {
        struct ui_tab_preview_source source;
        char *title;
        const struct ui_image *image;
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

void ui_tab_preview_init(struct ui_tab_preview *p);
/* Copies source. Its callback and userdata stay borrowed until replaced or
 * the preview is destroyed. */
void ui_tab_preview_set_source(struct ui_tab_preview *p,
                               const struct ui_tab_preview_source *source);
/* The pointer is over tab index, whose rect is anchor in surface space,
 * or over no tab with -1. */
void ui_tab_preview_hover(struct ui_tab_preview *p, int index,
                          struct rect2i anchor);
/* Hides the card until the pointer leaves the tabs, as after a press. */
void ui_tab_preview_suppress(struct ui_tab_preview *p);
/* Shows dragged tab index centered on at, in surface space. */
void ui_tab_preview_follow(struct ui_tab_preview *p, int index,
                           struct vector2i at);
void ui_tab_preview_hide(struct ui_tab_preview *p);
bool ui_tab_preview_is_visible(const struct ui_tab_preview *p);
/* The picture width the card wants, for a dragged tab or a hovered one. */
int ui_tab_preview_image_width(const struct ui_tab_preview *p, bool dragging);

static inline struct ui_widget *ui_tab_preview_widget(struct ui_tab_preview *p)
{
    return p ? &p->widget : NULL;
}

#endif
